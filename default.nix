{
  config,
  lib,
  pkgs,
  package ? null,
  ...
}:
let
  cfg = config.services.eternalTerminal;
  settingsFormat = pkgs.formats.ini { };
  defaultSettings = {
    Debug = {
      logdirectory = "/var/log/eternal-terminal";
      logsize = 20971520;
      silent = 0;
      telemetry = false;
      verbose = 0;
    };
  };
  mergedSettings = lib.recursiveUpdate defaultSettings cfg.settings;
  effectiveSettings = lib.recursiveUpdate mergedSettings {
    Networking.port = cfg.port;
  };
  configFile = settingsFormat.generate "et.cfg" effectiveSettings;
in
{
  options.services.eternalTerminal = {
    enable = lib.mkEnableOption "Eternal Terminal server";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = package;
      description = ''
        Package that provides `etserver`. When this module is imported through
        the flake, it defaults to that flake's package output.
      '';
    };

    port = lib.mkOption {
      type = lib.types.port;
      default = 2022;
      description = "TCP port for the Eternal Terminal server.";
    };

    openFirewall = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Open the configured Eternal Terminal port in the firewall.";
    };

    settings = lib.mkOption {
      type = settingsFormat.type;
      default = { };
      example = {
        Debug.serverfifo = "/run/etserver.fifo";
        Networking.bind_ip = "0.0.0.0";
      };
      description = ''
        Extra `et.cfg` settings grouped by INI section. This is merged with the
        module defaults, and `services.eternalTerminal.port` always wins for
        `Networking.port`.
      '';
    };
  };

  config = lib.mkIf cfg.enable (
    lib.mkMerge [
      {
        assertions = [
          {
            assertion = cfg.package != null;
            message = "services.eternalTerminal.package must be set when importing ./default.nix directly.";
          }
        ];

        environment.etc."et.cfg".source = configFile;
        networking.firewall.allowedTCPPorts = lib.mkIf cfg.openFirewall [ cfg.port ];
      }

      (lib.mkIf (cfg.package != null) {
        systemd.services."eternal-terminal" = {
          description = "Eternal Terminal";
          after = [ "network.target" ];
          wantedBy = [ "multi-user.target" ];

          serviceConfig = {
            ExecStart = "${cfg.package}/bin/etserver --cfgfile=/etc/et.cfg --logtostdout";
            LogsDirectory = "eternal-terminal";
            Restart = "on-failure";
            Type = "simple";
          };
        };
      })
    ]
  );
}
