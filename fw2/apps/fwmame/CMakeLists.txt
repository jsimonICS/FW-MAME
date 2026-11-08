add_executable(fwmame main.c usbhost/hh_hcd.c usbhost/hh_usb.c)

# The framebuffer design (fpga/fbuf) is built by wilifpga and embedded at
# build time, so the app always carries matching gateware.
set(FWMAME_REPO ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(FWMAME_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${FWMAME_GEN_DIR}/fbuf_fwfpga.h
    COMMAND ${Python3_EXECUTABLE} ${FWMAME_REPO}/tools/gen_fwfpga_header.py
            --name fbuf --out ${FWMAME_GEN_DIR}/fbuf_fwfpga.h
    DEPENDS ${FWMAME_REPO}/tools/gen_fwfpga_header.py
            ${FWMAME_REPO}/fpga/fbuf/user_core.v ${FWMAME_REPO}/fpga/fbuf/design.toml
    COMMENT "Building the fbuf FPGA design (wilifpga) and embedding it"
    VERBATIM)
add_custom_target(fwmame_gateware DEPENDS ${FWMAME_GEN_DIR}/fbuf_fwfpga.h)
add_dependencies(fwmame fwmame_gateware)

target_include_directories(fwmame PRIVATE ${FWMAME_GEN_DIR} ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(fwmame freewili2_bsp onewili_fwgui hardware_resets)
# SENSORS carries the I/O expander that gates the header VIO rail. The FPGA
# zone has no POWER_ZONES name; main.c holds it with picpwr.
fw2_display_app(fwmame
    POWER_ZONES SENSORS DISPLAY SDCARD USB_HUB AUDIO
    VERSION 003
    DESCRIPTION "FW-MAME: loads the framebuffer FPGA design and sends the FW2 buttons and a USB stick to the CM0"
    REPOSITORY "https://github.com/jsimonICS/FW-MAME")
