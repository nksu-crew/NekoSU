/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstddef>

constexpr const char *kKmsgPath = "/dev/kmsg";
constexpr std::size_t kLogLineMax = 512;

__attribute__((format(printf, 1, 2))) void klog(const char *fmt, ...);
void kmsg_write(const char *buf, std::size_t len);