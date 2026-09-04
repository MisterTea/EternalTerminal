-- Isolated WezTerm config for HTM GUI e2e. Loaded via --config-file so the
-- user's ~/.wezterm.lua is not used. Key bindings match the iTerm2 suite.
local wezterm = require 'wezterm'
local act = wezterm.action

return {
  check_for_updates = false,
  automatically_reload_config = false,
  audible_bell = 'Disabled',
  allow_win32_input_mode = false,
  disable_default_key_bindings = true,
  window_close_confirmation = 'NeverPrompt',
  skip_close_confirmation_for_processes_named = {
    'htm',
    'htm.exe',
    'htmd',
    'htmd.exe',
    'zsh',
    'bash',
    'sh',
    'fish',
    'powershell.exe',
    'pwsh.exe',
    'cmd.exe',
  },
  keys = {
    {
      key = 'd',
      mods = 'CMD',
      action = act.SplitHorizontal { domain = 'CurrentPaneDomain' },
    },
    {
      key = 'd',
      mods = 'CMD|SHIFT',
      action = act.SplitVertical { domain = 'CurrentPaneDomain' },
    },
    {
      key = 't',
      mods = 'CMD',
      action = act.SpawnTab 'CurrentPaneDomain',
    },
    {
      key = 'w',
      mods = 'CMD',
      action = act.CloseCurrentPane { confirm = false },
    },
    {
      key = '[',
      mods = 'CMD',
      action = act.ActivatePaneDirection 'Prev',
    },
    {
      key = ']',
      mods = 'CMD',
      action = act.ActivatePaneDirection 'Next',
    },
    {
      key = '[',
      mods = 'CMD|SHIFT',
      action = act.ActivateTabRelative(-1),
    },
    {
      key = ']',
      mods = 'CMD|SHIFT',
      action = act.ActivateTabRelative(1),
    },
    {
      key = 'd',
      mods = 'CTRL|SHIFT',
      action = act.SplitHorizontal { domain = 'CurrentPaneDomain' },
    },
    {
      key = 'd',
      mods = 'CTRL|ALT',
      action = act.SplitVertical { domain = 'CurrentPaneDomain' },
    },
    {
      key = 't',
      mods = 'CTRL|SHIFT',
      action = act.SpawnTab 'CurrentPaneDomain',
    },
    {
      key = 'w',
      mods = 'CTRL|SHIFT',
      action = act.CloseCurrentPane { confirm = false },
    },
    {
      key = 'h',
      mods = 'CTRL|SHIFT',
      action = act.ActivatePaneDirection 'Prev',
    },
    {
      key = 'l',
      mods = 'CTRL|SHIFT',
      action = act.ActivatePaneDirection 'Next',
    },
    {
      key = 'Tab',
      mods = 'CTRL|SHIFT',
      action = act.ActivateTabRelative(-1),
    },
  },
}
