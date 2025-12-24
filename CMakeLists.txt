
# 设置最低 CMake 版本要求
cmake_minimum_required(VERSION 3.10)

# 设置项目名称
project(KzAlloc)

# 设置C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项
# 开启 Debug 模式 + 地址消毒器 (AddressSanitizer)
#set(CMAKE_BUILD_TYPE Debug)
#add_compile_options(-g -O0 -fno-omit-frame-pointer) 

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -flto")
add_compile_options(-Wall -O3)

# 指定源文件目录下的所有 .cpp 文件
file(GLOB SOURCES "*.cpp")

find_package(Threads REQUIRED)

if(WIN32)
    # 如果是 Windows 平台
    # 判断使用的是 MSVC (Visual Studio) 还是 MinGW (GCC/Clang)
    if(MSVC)
        # MSVC 强制源码和执行字符集都为 UTF-8
        add_compile_options(/utf-8)
    else()
        # MinGW/GCC 强制执行字符集为 UTF-8 (源码默认通常就是 UTF-8)
        add_compile_options(-fexec-charset=UTF-8)
    endif()
endif()

# 设置目标可执行文件
add_executable(main ${SOURCES})
target_link_libraries(main PRIVATE Threads::Threads)

# 强烈建议开启 AddressSanitizer (ASan)，它是捕获内存越界的神器
# 检测编译器
#if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
#    target_compile_options(main PRIVATE -g -fno-omit-frame-pointer -fsanitize=address)
#    target_link_options(main PRIVATE -fsanitize=address)
#elseif(MSVC)
#   target_compile_options(main PRIVATE /fsanitize=address /Zi)
#    # MSVC 链接器通常自动处理，或者需要在 Linker Flags 关掉增量链接
#    set_property(TARGET main PROPERTY LINK_FLAGS "/INCREMENTAL:NO")
#endif()

# 清理中间的 .o 文件
set_target_properties(main PROPERTIES CLEAN_DIRECT_OUTPUT 1)

# 额外的编译选项（可根据需要启用）
# target_compile_options(main PRIVATE -Wall -Wextra -O2)