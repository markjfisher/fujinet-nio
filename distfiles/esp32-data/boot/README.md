Boot/config disk assets live here for ESP32 LittleFS uploadfs images.

Files in this directory are packaged into the ESP32 `storage` partition when
`./build.sh -f` runs through PlatformIO.

BBC FujiBus hosts use `bbc/autorun.ssd`; on ESP32 set:

```yaml
boot:
  mode: config
  config_uri: "flash:/boot/bbc/autorun.ssd"
  readonly: true
```
