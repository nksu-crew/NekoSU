#include <algorithm>
#include <iostream>
#include <string>
#include <sys/system_properties.h>
#include <vector>

int main(int argc, char *argv[]) {
  std::vector<std::pair<std::string, std::string>> properties;
  __system_property_foreach(
      [](const prop_info *pi, void *cookie) {
        __system_property_read_callback(
            pi,
            [](void *cookie, const char *name, const char *value, unsigned) {
              auto properties = reinterpret_cast<
                  std::vector<std::pair<std::string, std::string>> *>(cookie);
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