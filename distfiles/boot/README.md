Boot/config disk assets live here.

POSIX builds copy this directory to `fujinet-data/boot` next to the built app.
Place profile or machine-specific config disk images under subdirectories here
when they are added to the repository.

BBC FujiBus hosts use `bbc/autorun.ssd`; set:

```yaml
boot:
  mode: config
  config_uri: "persist:/boot/bbc/autorun.ssd"
  readonly: true
```
