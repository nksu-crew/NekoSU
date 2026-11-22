#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/futex.h>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// bionic property area layout.
//
// Every system property lives in a shared memory "property area".  Under
// /dev/__properties__ there is one file per SELinux context (plus
// "properties_serial" and "property_info").  bionic maps those files read-only
// into normal processes: only init, which runs the property service, owns a
// writable mapping.
//
// That is why __system_property_add()/__system_property_update() cannot be
// used here: called from a normal process they write through the read-only
// mapping and crash (and even init refuses to update an existing "ro."
// property).  To modify "ro." properties we therefore open the backing file
// read-write ourselves and apply bionic's writer protocol on the raw layout.
// Every other property is set through __system_property_set(), i.e. through
// init's property service, so its checks and triggers still apply.
//
// Layout references (AOSP bionic):
//   libc/system_properties/prop_area.h
//   libc/system_properties/prop_info.h
//   libc/system_properties/system_properties.cpp
//
// Note that bionic assumes init is the only writer of the property area.  The
// tool becomes a second writer; it is meant for short, one-shot updates (boot
// scripts, "hide my unlocked bootloader" style tweaks), not for hammering
// properties while the property service is busy adding hundreds of them.
// ---------------------------------------------------------------------------

constexpr char kPropertyAreaDir[] = PROP_DIRNAME "/";
constexpr char kSerialAreaFileName[] = "properties_serial";

constexpr size_t kPropertyAreaBytesUsedOffset = 0;
constexpr size_t kPropertyAreaSerialOffset = 4;
constexpr size_t kPropertyAreaMagicOffset = 8;
constexpr size_t kPropertyAreaVersionOffset = 12;

// sizeof(prop_area) without the flexible data_[] array.
constexpr size_t kPropertyAreaHeaderSize = 128;
// sizeof(prop_trie_node) without the flexible name[] array.
constexpr size_t kTrieNodeHeaderSize = 20;
// sizeof(prop_info) without the flexible name[] array.
constexpr size_t kPropInfoHeaderSize = 96;

constexpr size_t kTrieNodePropOffset = 4;
constexpr size_t kTrieNodeLeftOffset = 8;
constexpr size_t kTrieNodeRightOffset = 12;
constexpr size_t kTrieNodeChildrenOffset = 16;

constexpr size_t kPropInfoValueOffset = 4;
// offsetof(prop_info, long_property.offset)
constexpr size_t kPropInfoLongOffsetOffset = 4 + 56;

constexpr size_t kLongLegacyErrorBufferSize = 56;
constexpr char kLongLegacyError[] = "Must use __system_property_read_callback() to read";
constexpr size_t kLongLegacyErrorLength = sizeof(kLongLegacyError) - 1;

constexpr uint32_t kPropertyAreaMagic = 0x504f5250; // "PROP"
constexpr uint32_t kPropertyAreaVersion = 0xfc6ed0ab;

// prop_info::kLongFlag: serial bit telling readers that the value does not fit
// in the inline 92 byte buffer and lives elsewhere in the area.  bionic only
// allows such values for "ro." properties.
constexpr uint32_t kLongFlag = 1u << 16;
constexpr uint32_t kSerialCounterMask = 0x00ffffffu;
constexpr uint32_t kSerialCounterMaskWithoutLongFlag = kSerialCounterMask & ~kLongFlag;

static_assert(PROP_VALUE_MAX == 92,
	      "the prop_info layout hard-coded above assumes PROP_VALUE_MAX == 92");
static_assert(kLongLegacyErrorLength < kLongLegacyErrorBufferSize,
	      "legacy long property error message must fit into prop_info's buffer");

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------

size_t AlignUp4(size_t value)
{
	return (value + 3) & ~static_cast<size_t>(3);
}

bool CheckedRange(size_t offset, size_t length, size_t size)
{
	return offset <= size && length <= size - offset;
}

uint32_t LoadU32(const void *address)
{
	return __atomic_load_n(static_cast<const uint32_t *>(address), __ATOMIC_RELAXED);
}

void StoreU32(void *address, uint32_t value)
{
	__atomic_store_n(static_cast<uint32_t *>(address), value, __ATOMIC_RELAXED);
}

// bionic publishes trie pointers with release/consume ordering; acquire is the
// conservative equivalent on the reader side.
uint32_t LoadPublishedU32(const void *address)
{
	return __atomic_load_n(static_cast<const uint32_t *>(address), __ATOMIC_ACQUIRE);
}

void StorePublishedU32(void *address, uint32_t value)
{
	__atomic_store_n(static_cast<uint32_t *>(address), value, __ATOMIC_RELEASE);
}

void WakeFutex(uint32_t *address)
{
	syscall(SYS_futex, address, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

void BumpAreaSerial(uint32_t *serial)
{
	uint32_t previous = __atomic_load_n(serial, __ATOMIC_RELAXED);
	__atomic_store_n(serial, previous + 1, __ATOMIC_RELEASE);
	WakeFutex(serial);
}

std::string SerialAreaPath()
{
	return std::string(kPropertyAreaDir) + kSerialAreaFileName;
}

bool IsPropertyAreaFile(const std::string &path)
{
	constexpr size_t prefixLength = sizeof(kPropertyAreaDir) - 1;
	return path.size() > prefixLength && path.compare(0, prefixLength, kPropertyAreaDir) == 0;
}

// ---------------------------------------------------------------------------
// Read-only property access through bionic.  These are public NDK APIs and
// always observe the shared memory, even the areas we modify ourselves.
// ---------------------------------------------------------------------------

bool ReadPropertyValue(const std::string &name, std::string *value)
{
	const prop_info *info = __system_property_find(name.c_str());
	if (info == nullptr) {
		return false;
	}

	std::string result;
	__system_property_read_callback(
		info,
		[](void *cookie, const char *, const char *propValue, uint32_t) {
			*static_cast<std::string *>(cookie) = propValue;
		},
		&result);
	*value = std::move(result);
	return true;
}

void ShowAllProperties()
{
	std::vector<std::pair<std::string, std::string>> properties;
	__system_property_foreach(
		[](const prop_info *info, void *cookie) {
			__system_property_read_callback(
				info,
				[](void *cookie, const char *name, const char *value, uint32_t) {
					auto properties = reinterpret_cast<
						std::vector<std::pair<std::string, std::string>> *>(
						cookie);
					properties->emplace_back(name, value);
				},
				cookie);
		},
		&properties);

	std::sort(properties.begin(), properties.end());

	for (const auto &[name, value] : properties) {
		std::cout << "[" << name << "]: [" << value << "]" << std::endl;
	}
}

// ---------------------------------------------------------------------------
// /proc/self/maps: used to locate the file backing a prop_info pointer that
// bionic handed out.
// ---------------------------------------------------------------------------

struct Mapping
{
	uintptr_t start = 0;
	uintptr_t end = 0;
	uint64_t offset = 0;
	std::string path;
};

bool ParseSelfMaps(std::vector<Mapping> &mappings)
{
	std::ifstream maps("/proc/self/maps");
	if (!maps) {
		return false;
	}

	std::string line;
	while (std::getline(maps, line)) {
		std::istringstream fields(line);
		std::string range;
		std::string permissions;
		std::string offset;
		std::string device;
		std::string inode;
		if (!(fields >> range >> permissions >> offset >> device >> inode)) {
			continue;
		}

		unsigned long start = 0;
		unsigned long end = 0;
		if (std::sscanf(range.c_str(), "%lx-%lx", &start, &end) != 2) {
			continue;
		}

		std::string path;
		std::getline(fields, path);
		size_t begin = path.find_first_not_of(" \t");
		if (begin == std::string::npos) {
			continue;
		}

		Mapping mapping;
		mapping.start = static_cast<uintptr_t>(start);
		mapping.end = static_cast<uintptr_t>(end);
		mapping.offset = std::strtoull(offset.c_str(), nullptr, 16);
		mapping.path = path.substr(begin);
		mappings.push_back(std::move(mapping));
	}
	return true;
}

const Mapping *FindMapping(const std::vector<Mapping> &mappings, const void *address)
{
	uintptr_t value = reinterpret_cast<uintptr_t>(address);
	for (const auto &mapping : mappings) {
		if (value >= mapping.start && value < mapping.end) {
			return &mapping;
		}
	}
	return nullptr;
}

// A lookup for a property that does not exist yet makes bionic map the property
// area file that would hold it.  Comparing the maps before and after that
// lookup therefore reveals the file to write to.
const Mapping *FindNewPropertyArea(const std::vector<Mapping> &before,
				   const std::vector<Mapping> &after)
{
	for (const auto &mapping : after) {
		if (!IsPropertyAreaFile(mapping.path)) {
			continue;
		}
		bool existed = false;
		for (const auto &previous : before) {
			if (previous.start == mapping.start && previous.path == mapping.path) {
				existed = true;
				break;
			}
		}
		if (!existed) {
			return &mapping;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Read-write mapping of a property area file.
// ---------------------------------------------------------------------------

class WritableMapping
{
public:
	WritableMapping() = default;
	WritableMapping(const WritableMapping &) = delete;
	WritableMapping &operator=(const WritableMapping &) = delete;
	~WritableMapping()
	{
		Reset();
	}

	bool Open(const std::string &path, std::string *error)
	{
		Reset();

		int fd = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) {
			*error = "cannot open " + path + " read-write: " + std::strerror(errno);
			return false;
		}

		struct stat status {};
		if (fstat(fd, &status) < 0) {
			int saved = errno;
			close(fd);
			*error = "cannot stat " + path + ": " + std::strerror(saved);
			return false;
		}
		if (!S_ISREG(status.st_mode) ||
		    status.st_size < static_cast<off_t>(kPropertyAreaHeaderSize + kTrieNodeHeaderSize)) {
			close(fd);
			*error = path + " is not a property area";
			return false;
		}

		size_t size = static_cast<size_t>(status.st_size);
		void *memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		int saved = errno;
		close(fd);
		if (memory == MAP_FAILED) {
			*error = "cannot map " + path + " writable: " + std::strerror(saved);
			return false;
		}

		address_ = static_cast<uint8_t *>(memory);
		size_ = size;
		return true;
	}

	uint8_t *Data() const
	{
		return address_;
	}

	size_t Size() const
	{
		return size_;
	}

private:
	void Reset()
	{
		if (address_ != nullptr) {
			munmap(address_, size_);
			address_ = nullptr;
			size_ = 0;
		}
	}

	uint8_t *address_ = nullptr;
	size_t size_ = 0;
};

bool ValidatePropertyArea(const uint8_t *area, size_t size, std::string *error)
{
	if (size < kPropertyAreaHeaderSize + kTrieNodeHeaderSize) {
		*error = "property area is too small";
		return false;
	}
	if (LoadU32(area + kPropertyAreaMagicOffset) != kPropertyAreaMagic ||
	    LoadU32(area + kPropertyAreaVersionOffset) != kPropertyAreaVersion) {
		*error = "invalid property area header (broken magic/version)";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Property area writer.  Mirrors the logic behind prop_area::add() and
// SystemProperties::Update() in bionic.
// ---------------------------------------------------------------------------

bool HasDirtyBackupArea(const uint8_t *area, size_t dataSize)
{
	if (dataSize < kTrieNodeHeaderSize) {
		return false;
	}
	const uint8_t *root = area + kPropertyAreaHeaderSize;
	uint32_t children = LoadU32(root + kTrieNodeChildrenOffset);
	if (children == kTrieNodeHeaderSize) {
		// The first node starts right after the root node: no dirty backup area.
		return false;
	}
	if (children == 0) {
		// Empty area: a modern one reserves the backup area right away.
		return LoadU32(area + kPropertyAreaBytesUsedOffset) == kTrieNodeHeaderSize + PROP_VALUE_MAX;
	}
	return true;
}

bool AllocateData(uint8_t *area, size_t dataSize, size_t size, uint32_t *offset, std::string *error)
{
	size_t aligned = AlignUp4(size);
	uint32_t used = LoadU32(area + kPropertyAreaBytesUsedOffset);
	if (used > dataSize || aligned > dataSize - used) {
		*error = "property area is full";
		return false;
	}

	*offset = used;
	StoreU32(area + kPropertyAreaBytesUsedOffset, static_cast<uint32_t>(used + aligned));
	// bionic relies on fresh mmap pages being zero, but the area may have been
	// recycled, so make the new object deterministic.
	std::memset(area + kPropertyAreaHeaderSize + *offset, 0, aligned);
	return true;
}

void InitTrieNode(uint8_t *area, uint32_t offset, const char *name, size_t nameLength)
{
	uint8_t *node = area + kPropertyAreaHeaderSize + offset;
	StoreU32(node, static_cast<uint32_t>(nameLength));
	StoreU32(node + kTrieNodePropOffset, 0);
	StoreU32(node + kTrieNodeLeftOffset, 0);
	StoreU32(node + kTrieNodeRightOffset, 0);
	StoreU32(node + kTrieNodeChildrenOffset, 0);
	std::memcpy(node + kTrieNodeHeaderSize, name, nameLength);
}

uint32_t LoadTriePointer(const uint8_t *area, uint32_t nodeOffset, size_t fieldOffset)
{
	return LoadPublishedU32(area + kPropertyAreaHeaderSize + nodeOffset + fieldOffset);
}

void StoreTriePointer(uint8_t *area, uint32_t nodeOffset, size_t fieldOffset, uint32_t value)
{
	StorePublishedU32(area + kPropertyAreaHeaderSize + nodeOffset + fieldOffset, value);
}

// prop_area::cmp_prop_name(): compare by length first, then by content.
int CompareNamePart(const char *one, size_t oneLength, const char *two, size_t twoLength)
{
	if (oneLength < twoLength) {
		return -1;
	}
	if (oneLength > twoLength) {
		return 1;
	}
	if (oneLength == 0) {
		return 0;
	}
	return std::strncmp(one, two, oneLength);
}

// prop_area::find_prop_trie_node(), with the same allocation behaviour.
bool FindTrieNode(uint8_t *area, size_t dataSize, uint32_t rootOffset, const char *name,
		  size_t nameLength, bool allocate, uint32_t *result, std::string *error)
{
	uint32_t current = rootOffset;
	for (;;) {
		if (!CheckedRange(current, kTrieNodeHeaderSize, dataSize)) {
			*error = "corrupted property trie (node offset out of range)";
			return false;
		}

		uint32_t nodeNameLength = LoadU32(area + kPropertyAreaHeaderSize + current);
		if (!CheckedRange(current + kTrieNodeHeaderSize, nodeNameLength, dataSize)) {
			*error = "corrupted property trie (node name out of range)";
			return false;
		}
		const char *nodeName =
			reinterpret_cast<const char *>(area + kPropertyAreaHeaderSize + current +
						       kTrieNodeHeaderSize);

		int comparison = CompareNamePart(name, nameLength, nodeName, nodeNameLength);
		if (comparison == 0) {
			*result = current;
			return true;
		}

		size_t field = comparison < 0 ? kTrieNodeLeftOffset : kTrieNodeRightOffset;
		uint32_t child = LoadTriePointer(area, current, field);
		if (child != 0) {
			current = child;
			continue;
		}

		if (!allocate) {
			*error = "property not found";
			return false;
		}

		uint32_t created = 0;
		if (!AllocateData(area, dataSize, kTrieNodeHeaderSize + nameLength + 1, &created, error)) {
			return false;
		}
		InitTrieNode(area, created, name, nameLength);
		StoreTriePointer(area, current, field, created);
		*result = created;
		return true;
	}
}

bool UpdatePropInfo(uint8_t *area, size_t areaSize, uint32_t dataOffset, const std::string &name,
		    const std::string &value, uint32_t *areaSerial, std::string *error)
{
	size_t dataSize = areaSize - kPropertyAreaHeaderSize;
	if (!CheckedRange(dataOffset, kPropInfoHeaderSize + 1, dataSize)) {
		*error = "corrupted property area (prop_info offset out of range)";
		return false;
	}

	uint8_t *prop = area + kPropertyAreaHeaderSize + dataOffset;
	const char *propName = reinterpret_cast<const char *>(prop + kPropInfoHeaderSize);
	size_t nameLimit = dataSize - dataOffset - kPropInfoHeaderSize;
	size_t propNameLength = strnlen(propName, nameLimit);
	if (propNameLength == 0 || propNameLength == nameLimit) {
		*error = "corrupted property area (unterminated property name)";
		return false;
	}
	if (propNameLength != name.size() || std::memcmp(propName, name.data(), name.size()) != 0) {
		*error = "property area lookup mismatch for \"" + name + "\"";
		return false;
	}

	size_t valueLength = value.size();
	bool longValue = valueLength >= static_cast<size_t>(PROP_VALUE_MAX);
	if (longValue && std::strncmp(propName, "ro.", 3) != 0) {
		*error = "values longer than " + std::to_string(PROP_VALUE_MAX - 1) +
			 " bytes are only supported for ro. properties";
		return false;
	}

	uint32_t *serial = reinterpret_cast<uint32_t *>(prop);
	uint32_t oldSerial = LoadU32(serial);
	bool oldLong = (oldSerial & kLongFlag) != 0;

	// Prepare long value storage first: readers only look there once the serial
	// says the property is long, so it can be filled before the dirty bit is
	// published.  The inline value, in contrast, must only be overwritten after
	// the dirty bit is visible.
	uint32_t longValueOffset = 0;
	bool reuseLongStorage = false;
	if (longValue && oldLong) {
		uint32_t previousRelative = LoadU32(prop + kPropInfoLongOffsetOffset);
		// The offset is relative to this prop_info and always points past the
		// record itself for areas written by bionic.  Reuse the slot when the new
		// value fits so a repeated write does not keep growing the area.
		size_t minimumRelative = kPropInfoHeaderSize + propNameLength + 1;
		if (previousRelative >= minimumRelative && previousRelative <= dataSize &&
		    dataOffset <= dataSize - previousRelative) {
			size_t previousOffset = static_cast<size_t>(dataOffset) + previousRelative;
			const char *previousValue = reinterpret_cast<const char *>(
				area + kPropertyAreaHeaderSize + previousOffset);
			size_t previousLength = strnlen(previousValue, dataSize - previousOffset);
			if (previousLength < dataSize - previousOffset && valueLength <= previousLength) {
				reuseLongStorage = true;
				longValueOffset = static_cast<uint32_t>(previousOffset);
			}
		}
	}

	if (longValue && !reuseLongStorage) {
		if (!AllocateData(area, dataSize, valueLength + 1, &longValueOffset, error)) {
			return false;
		}
	}
	if (longValue) {
		uint8_t *storage = area + kPropertyAreaHeaderSize + longValueOffset;
		std::memcpy(storage, value.data(), valueLength);
		storage[valueLength] = '\0';
	}

	// Keep a consistent pre-update value in the dirty backup area for readers
	// that observe the dirty serial (bionic's wait-free read protocol).
	if (HasDirtyBackupArea(area, dataSize)) {
		size_t oldLength = oldSerial >> 24;
		if (oldLength >= PROP_VALUE_MAX) {
			oldLength = PROP_VALUE_MAX - 1;
		}
		std::memcpy(area + kPropertyAreaHeaderSize + kTrieNodeHeaderSize,
			    prop + kPropInfoValueOffset, oldLength + 1);
	}
	__atomic_thread_fence(__ATOMIC_RELEASE);

	uint32_t dirtySerial = oldSerial | 1u;
	StoreU32(serial, dirtySerial);

	std::memset(prop + kPropInfoValueOffset, 0, PROP_VALUE_MAX);
	if (longValue) {
		std::memcpy(prop + kPropInfoValueOffset, kLongLegacyError, sizeof(kLongLegacyError));
		StoreU32(prop + kPropInfoLongOffsetOffset, longValueOffset - dataOffset);
	} else {
		std::memcpy(prop + kPropInfoValueOffset, value.data(), valueLength);
	}
	__atomic_thread_fence(__ATOMIC_RELEASE);

	uint32_t serialLength =
		longValue ? static_cast<uint32_t>(kLongLegacyErrorLength) : static_cast<uint32_t>(valueLength);
	uint32_t lengthFlags = (serialLength << 24) | (longValue ? kLongFlag : 0u);
	StoreU32(serial, lengthFlags | ((dirtySerial + 1) & kSerialCounterMaskWithoutLongFlag));
	WakeFutex(serial);
	BumpAreaSerial(areaSerial);

	// Restore the serial counter so that the modification is not visible through
	// __system_property_serial(); only the length bits (and long-ness) change.
	// This keeps the property looking like the boot-time value it replaced.
	__atomic_thread_fence(__ATOMIC_RELEASE);
	StoreU32(serial, lengthFlags | ((dirtySerial & ~1u) & kSerialCounterMaskWithoutLongFlag));
	return true;
}

bool InsertPropInfo(uint8_t *area, size_t areaSize, const std::string &name,
		    const std::string &value, uint32_t *areaSerial, std::string *error)
{
	size_t dataSize = areaSize - kPropertyAreaHeaderSize;

	// Walk the trie, creating the missing nodes like prop_area::find_property()
	// does when it is asked to allocate.
	uint32_t current = 0;
	const char *remaining = name.data();
	size_t remainingLength = name.size();
	for (;;) {
		size_t partLength = remainingLength;
		bool wantSubtree = false;
		for (size_t i = 0; i < remainingLength; ++i) {
			if (remaining[i] == '.') {
				partLength = i;
				wantSubtree = true;
				break;
			}
		}
		if (partLength == 0) {
			*error = "invalid property name";
			return false;
		}

		uint32_t children = LoadTriePointer(area, current, kTrieNodeChildrenOffset);
		if (children == 0) {
			if (!AllocateData(area, dataSize, kTrieNodeHeaderSize + partLength + 1, &children,
					  error)) {
				return false;
			}
			InitTrieNode(area, children, remaining, partLength);
			StoreTriePointer(area, current, kTrieNodeChildrenOffset, children);
		}

		uint32_t node = 0;
		if (!FindTrieNode(area, dataSize, children, remaining, partLength, true, &node, error)) {
			return false;
		}
		current = node;

		if (!wantSubtree) {
			break;
		}
		remaining += partLength + 1;
		remainingLength -= partLength + 1;
	}

	uint32_t existing = LoadTriePointer(area, current, kTrieNodePropOffset);
	if (existing != 0) {
		return UpdatePropInfo(area, areaSize, existing, name, value, areaSerial, error);
	}

	uint32_t propOffset = 0;
	if (!AllocateData(area, dataSize, kPropInfoHeaderSize + name.size() + 1, &propOffset, error)) {
		return false;
	}

	bool longValue = value.size() >= static_cast<size_t>(PROP_VALUE_MAX);
	uint32_t longValueOffset = 0;
	if (longValue) {
		if (std::strncmp(name.data(), "ro.", 3) != 0) {
			*error = "values longer than " + std::to_string(PROP_VALUE_MAX - 1) +
				 " bytes are only supported for ro. properties";
			return false;
		}
		if (!AllocateData(area, dataSize, value.size() + 1, &longValueOffset, error)) {
			return false;
		}
	}

	uint8_t *prop = area + kPropertyAreaHeaderSize + propOffset;
	// The name follows the fixed prop_info header; allocation zeroed the bytes,
	// so the terminating NUL is already in place.
	std::memcpy(prop + kPropInfoHeaderSize, name.data(), name.size());

	uint32_t serial = 0;
	if (longValue) {
		uint8_t *storage = area + kPropertyAreaHeaderSize + longValueOffset;
		std::memcpy(storage, value.data(), value.size());
		std::memcpy(prop + kPropInfoValueOffset, kLongLegacyError, sizeof(kLongLegacyError));
		StoreU32(prop + kPropInfoLongOffsetOffset, longValueOffset - propOffset);
		serial = (static_cast<uint32_t>(kLongLegacyErrorLength) << 24) | kLongFlag;
	} else {
		std::memcpy(prop + kPropInfoValueOffset, value.data(), value.size());
		serial = static_cast<uint32_t>(value.size()) << 24;
	}
	StoreU32(prop, serial);

	// Publish the prop_info to readers only once it is fully initialised.
	__atomic_thread_fence(__ATOMIC_RELEASE);
	StoreTriePointer(area, current, kTrieNodePropOffset, propOffset);

	BumpAreaSerial(areaSerial);
	return true;
}

// ---------------------------------------------------------------------------
// Command line operations.
// ---------------------------------------------------------------------------

bool IsValidPropertyName(const std::string &name, std::string *error)
{
	// Same rules as init's IsLegalPropertyName() (system/core/init/util.cpp).
	// PROP_NAME_MAX is deliberately not enforced: init itself creates ro.*
	// properties that are longer than it (ro.appcompat_override.*).
	if (name.empty()) {
		*error = "property name is empty";
		return false;
	}
	if (name.front() == '.' || name.back() == '.') {
		*error = "property name must not start or end with '.'";
		return false;
	}
	for (size_t i = 0; i < name.size(); ++i) {
		char character = name[i];
		if (character == '.') {
			if (name[i - 1] == '.') {
				*error = "property name must not contain \"..\"";
				return false;
			}
			continue;
		}
		if (character == '_' || character == '-' || character == '@' || character == ':') {
			continue;
		}
		if ((character >= 'a' && character <= 'z') ||
		    (character >= 'A' && character <= 'Z') ||
		    (character >= '0' && character <= '9')) {
			continue;
		}
		*error = "illegal character in property name";
		return false;
	}
	if (name.compare(0, 4, "ctl.") == 0) {
		*error = "ctl.* properties are handled by init and cannot be set directly";
		return false;
	}
	return true;
}

bool SetProperty(const std::string &name, const std::string &value, std::string *error)
{
	if (!IsValidPropertyName(name, error)) {
		return false;
	}

	// Non-ro properties go through the property service, exactly like
	// setprop(1): init enforces the SELinux and type checks, persists persist.*
	// values and runs the property triggers.  Only ro.* properties are written
	// directly below, because the service refuses to update them a second time.
	if (!name.starts_with("ro.")) {
		if (value.size() >= static_cast<size_t>(PROP_VALUE_MAX)) {
			*error = "values longer than " + std::to_string(PROP_VALUE_MAX - 1) +
				 " bytes are only supported for ro. properties";
			return false;
		}
		errno = 0;
		int result = __system_property_set(name.c_str(), value.c_str());
		if (result != 0) {
			int saved = errno;
			*error = "property service rejected \"" + name + "\"";
			if (saved != 0) {
				*error += ": ";
				*error += std::strerror(saved);
			}
			return false;
		}
		return true;
	}

	std::vector<Mapping> mapsBefore;
	if (!ParseSelfMaps(mapsBefore)) {
		*error = "cannot read /proc/self/maps";
		return false;
	}

	// Ask bionic where the property lives.  When it does not exist yet this also
	// makes bionic map the area that would hold it, which is how we find the
	// file below.
	const prop_info *info = __system_property_find(name.c_str());

	std::vector<Mapping> mapsAfter;
	if (!ParseSelfMaps(mapsAfter)) {
		*error = "cannot read /proc/self/maps";
		return false;
	}

	const Mapping *areaMapping = nullptr;
	uint32_t dataOffset = 0;
	if (info != nullptr) {
		areaMapping = FindMapping(mapsAfter, info);
		if (areaMapping == nullptr || !IsPropertyAreaFile(areaMapping->path) ||
		    areaMapping->offset != 0) {
			*error = "cannot locate the property area backing \"" + name + "\"";
			return false;
		}
		uintptr_t infoAddress = reinterpret_cast<uintptr_t>(info);
		if (infoAddress < areaMapping->start + kPropertyAreaHeaderSize) {
			*error = "property area mapping is inconsistent";
			return false;
		}
		dataOffset = static_cast<uint32_t>(infoAddress - areaMapping->start -
						   kPropertyAreaHeaderSize);
	} else {
		areaMapping = FindNewPropertyArea(mapsBefore, mapsAfter);
		if (areaMapping == nullptr) {
			*error = "cannot resolve the property area for \"" + name +
				 "\" (unknown property context)";
			return false;
		}
	}
	if (areaMapping->offset != 0) {
		*error = "unexpected property area mapping offset";
		return false;
	}

	WritableMapping area;
	if (!area.Open(areaMapping->path, error) ||
	    !ValidatePropertyArea(area.Data(), area.Size(), error)) {
		return false;
	}

	WritableMapping serialArea;
	if (!serialArea.Open(SerialAreaPath(), error) ||
	    !ValidatePropertyArea(serialArea.Data(), serialArea.Size(), error)) {
		return false;
	}
	uint32_t *areaSerial = reinterpret_cast<uint32_t *>(serialArea.Data() + kPropertyAreaSerialOffset);

	bool updated;
	if (info != nullptr) {
		updated = UpdatePropInfo(area.Data(), area.Size(), dataOffset, name, value, areaSerial,
					 error);
	} else {
		updated = InsertPropInfo(area.Data(), area.Size(), name, value, areaSerial, error);
	}
	if (!updated) {
		return false;
	}

	// Fast-fail: surface a write that did not actually take effect.
	std::string actual;
	if (!ReadPropertyValue(name, &actual)) {
		*error = "property \"" + name + "\" disappeared after the update";
		return false;
	}
	if (actual != value) {
		*error = "verification failed for \"" + name + "\": expected \"" + value + "\", got \"" +
			 actual + "\"";
		return false;
	}
	return true;
}

} // namespace

int main(int argc, char *argv[])
{
	if (argc < 2) {
		ShowAllProperties();
		return 0;
	}
	if (argc == 2) {
		std::string value;
		if (!ReadPropertyValue(argv[1], &value)) {
			std::cout << argv[0] << ": "
				  << "Property not found." << std::endl;
			return 1;
		}
		std::cout << "[" << argv[1] << "]: [" << value << "]" << std::endl;
		return 0;
	}
	if (argc > 2) {
		std::string error;
		if (!SetProperty(argv[1], argv[2], &error)) {
			std::cerr << argv[0] << ": " << error << std::endl;
			return 1;
		}
		return 0;
	}
	return 0;
}
