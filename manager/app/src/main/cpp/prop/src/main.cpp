#include <algorithm>
#include <dlfcn.h>
#include <iostream>
#include <string>
#include <sys/system_properties.h>
#include <vector>

namespace
{

using system_property_add_t = int (*)(const char *name, unsigned int namelen, const char *value,
				      unsigned int valuelen);
using system_property_update_t = int (*)(prop_info *pi, const char *value, unsigned int len);

system_property_add_t get_property_add()
{
	static system_property_add_t fn = []() -> system_property_add_t {
		auto p = reinterpret_cast<system_property_add_t>(
			dlsym(RTLD_DEFAULT, "__system_property_add"));
		if (p == nullptr) {
			void *libc = dlopen("libc.so", RTLD_LAZY | RTLD_NOLOAD);
			if (libc != nullptr) {
				p = reinterpret_cast<system_property_add_t>(
					dlsym(libc, "__system_property_add"));
				dlclose(libc);
			}
		}
		return p;
	}();
	return fn;
}

system_property_update_t get_property_update()
{
	static system_property_update_t fn = []() -> system_property_update_t {
		auto p = reinterpret_cast<system_property_update_t>(
			dlsym(RTLD_DEFAULT, "__system_property_update"));
		if (p == nullptr) {
			void *libc = dlopen("libc.so", RTLD_LAZY | RTLD_NOLOAD);
			if (libc != nullptr) {
				p = reinterpret_cast<system_property_update_t>(
					dlsym(libc, "__system_property_update"));
				dlclose(libc);
			}
		}
		return p;
	}();
	return fn;
}

void ShowAllProperties(void)
{
	std::vector<std::pair<std::string, std::string>> properties;
	__system_property_foreach(
		[](const prop_info *pi, void *cookie) {
			__system_property_read_callback(
				pi,
				[](void *cookie, const char *name, const char *value, unsigned) {
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

} // namespace

int main(int argc, char *argv[])
{
	if (argc < 2) {
		ShowAllProperties();
		return 0;
	}
	if (argc == 2) {
		char value[PROP_VALUE_MAX];
		if ((__system_property_get(argv[1], value)) < 0) {
			std::cout << argv[0] << ": " << "Property not found." << std::endl;
		} else {
			std::cout << "[" << argv[1] << "]: [" << value << "]" << std::endl;
		}
		return 0;
	}
	if (argc > 2) {
		std::string name = argv[1];
		std::string value = argv[2];
		size_t valuelen = value.size();

		prop_info *pi = (prop_info *)__system_property_find(name.c_str());
		if (pi != nullptr) {
			auto update_fn = get_property_update();
			if (update_fn == nullptr) {
				std::cout << "__system_property_update not available" << std::endl;
				return -1;
			}
			int rc = update_fn(pi, value.c_str(), valuelen);
			if (rc < 0) {
				std::cout << "__system_property_update failed" << std::endl;
				return -1;
			}
		} else {
			auto add_fn = get_property_add();
			if (add_fn == nullptr) {
				std::cout << "__system_property_add not available" << std::endl;
				return -1;
			}
			int rc = add_fn(name.c_str(), name.size(), value.c_str(), valuelen);
			if (rc < 0) {
				std::cout << "__system_property_add failed" << std::endl;
				return -1;
			}
		}
		return 0;
	}
	return 0;
}