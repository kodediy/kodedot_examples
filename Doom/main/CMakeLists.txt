idf_component_register(
    SRCS "main.c" "doomgeneric_kode.c" "doom_display.c" "doom_blit.c" "doom_input.c"
         "doom_wad.c" "doom_wadfile_mem.c" "doom_sound.c" "doom_haptic.c"
    INCLUDE_DIRS "."
    REQUIRES esp_system doomgeneric esp_lcd esp_driver_i2c esp_io_expander
    PRIV_REQUIRES esp_timer esp_hw_support esp_driver_gpio esp_io_expander_tca9554 esp_lcd_co5300
                  esp_mm esp_driver_ppa fatfs esp_driver_sdmmc sdmmc esp_hosted esp_driver_i2s
                  esp_codec_dev
    EMBED_FILES "doom1.wad"
)
# doom1.wad (shareware 1.9, redistribuible integro y gratis) va EMBEBIDO y la app
# lo siembra en /sd/doom/ si no hay ningun IWAD. No se versiona (4,2 MB): hay que
# copiarlo a main/doom1.wad ANTES de compilar o el build falla aqui mismo.
# MD5 esperado: f0cefca49926d00903cf57551d901abe. Ver el README.
