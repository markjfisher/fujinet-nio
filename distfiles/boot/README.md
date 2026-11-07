Boot/config disk assets are generated here.

POSIX builds copy this directory to `fujinet-data/boot` next to the built app.
Do not commit generated `.atr`, `.img`, or `.ssd` files from this directory.
Generate them from the workspace boot-disk build tasks when packaging a local
build or release.

BBC FujiBus hosts use `bbc/FN-BOOT.ssd`; set:

```yaml
boot:
  mode: config
  config_uri: "persist:/boot/bbc/FN-BOOT.ssd"
  readonly: true
```

The C64 boot disk is `c64/autorun.d64`, from `nio-core-apps`. The `c64-netiec`
profiles look for it there without any configuration.

Amiga default disks are profile-specific FFS/DD ADFs.  For a POSIX FujiNet
instance, select one explicitly, for example:

```yaml
boot:
  mode: config
  config_uri: "persist:/boot/amiga/wb32/FujiNet-Default-WB32.adf"
  readonly: true
```
