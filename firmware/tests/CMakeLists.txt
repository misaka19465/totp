# 主机侧单元测试：只编译 app/ 下的纯计算模块，不依赖 CMSIS/寄存器。
# 用法：cmake -S firmware/tests -B firmware/tests/build && cmake --build firmware/tests/build && ctest --test-dir firmware/tests/build
cmake_minimum_required(VERSION 3.20)

project(totp_token_host_tests LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_executable(test_app
  test_main.c
  fake_ht1621.c
  fake_plug_detect.c
  fake_usart2_hw.c
  test_base32.c
  test_hmac_sha1.c
  test_lcd_render.c
  test_totp.c
  test_uart_link.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/app/base32.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/app/hmac_sha1.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/app/totp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/drivers/lcd_render.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/drivers/uart_link.c
)

target_include_directories(test_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/app
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/drivers
)

# 与固件同一套告警契约，保证测试代码也按交叉编译的口径干净通过
target_compile_options(test_app PRIVATE
  -Wall -Wextra -Wshadow -Wundef -Wdouble-promotion -Wswitch-enum -Werror
)

add_test(NAME app_vectors COMMAND test_app)
