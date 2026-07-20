Boot/config disk assets are generated here for ESP32 LittleFS uploadfs images.

Files in this directory are packaged into the ESP32 `storage` partition when
`./build.sh -f` runs through PlatformIO.
Do not commit generated `.atr`, `.img`, or `.ssd` files from this directory.
Generate them from the workspace boot-disk build tasks when packaging a local
build or release.

BBC FujiBus hosts use `bbc/autorun.ssd`; on ESP32 set:

```yaml
boot:
  mode: config
  config_uri: "flash:/boot/bbc/autorun.ssd"
  readonly: true
```
