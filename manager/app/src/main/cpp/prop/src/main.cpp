#include <algorithm>
#include <iostream>
#include <string>
#include <sys/system_properties.h>
#include <vector>

namespace
{

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
			__system_property_update(pi, value.c_str(), valuelen);
		} else {
			int rc = __system_property_add(name.c_str(), name.size(), value.c_str(),
						       valuelen);
			if (rc < 0) {
				std::cout << "__system_property_add failed" << std::endl;
				return -1;
			}
		}
		return 0;
	}
}