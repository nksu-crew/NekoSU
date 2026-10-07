// kmod.h
#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * 加载单个内核模块 image。
 * 会自动解析 /proc/kallsyms 完成符号修补 (ksym 替换为绝对地址)。
 * 成功返回 0, 失败返回负 errno。
 */
int kmod_load(const void *image, size_t size);
