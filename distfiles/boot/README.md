Boot/config disk assets are generated here.

POSIX builds copy this directory to `fujinet-data/boot` next to the built app.
Do not commit generated `.atr`, `.img`, or `.ssd` files from this directory.
Generate them from the workspace boot-disk build tasks when packaging a local
build or release.

BBC FujiBus hosts use `bbc/autorun.ssd`; set:

```yaml
boot:
  mode: config
  config_uri: "persist:/boot/bbc/autorun.ssd"
  readonly: true
```
