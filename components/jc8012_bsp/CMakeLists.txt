# Local Kconfig shim for the Guition JC8012P4A1C board.  The board has no
# upstream ESP-BSP package, but the shared sdkconfig.defaults overlay sets
# several CONFIG_BSP_* symbols.  This header-only component defines those
# symbols so that confgen can resolve them for the panel10jc variant.
idf_component_register()
