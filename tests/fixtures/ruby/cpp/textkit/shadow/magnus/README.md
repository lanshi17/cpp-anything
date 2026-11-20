# magnus transport for `textkit`

`./build.sh` builds the Ruby native extension (`magnus`) over the same
C ABI the Fiddle path uses and copies it next to the shadow wrapper; the
wrapper prefers it when `textkit_shadow_ext.so` is present. It needs cargo and a Ruby
with headers (`ruby-dev` / `xcode-select --install`). Without them, the wrapper
keeps working through Fiddle.
