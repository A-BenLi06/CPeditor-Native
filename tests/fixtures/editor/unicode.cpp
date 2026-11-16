// SPDX-License-Identifier: GPL-3.0-or-later
// 中文输入、emoji 😀 与组合字符 é 的编辑测试文件。
#include <iostream>
#include <string>

int main() {
    long long left = 0;
    long long right = 0;
    if (std::cin >> left >> right) {
        std::cout << left + right << '\n';
    }
    return 0;
}
