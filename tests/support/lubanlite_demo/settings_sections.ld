SECTIONS
{
  .settings_handlers : ALIGN(8)
  {
    _settings_handler_static_list_start = .;
    KEEP(*(SORT_BY_NAME(._settings_handler_static.static.*)))
    _settings_handler_static_list_end = .;
  }
}
INSERT AFTER .data;
