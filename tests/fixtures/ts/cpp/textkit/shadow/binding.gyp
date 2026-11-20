{
  "targets": [
    {
      "target_name": "textkit_shadow",
      "sources": ["addon.cc"],
      "libraries": ["-L/mnt/data/Projects/OpenSource/cpp-anything/tests/fixtures/ts/cpp/textkit/build", "-lcppan_shadow", "-Wl,-rpath,/mnt/data/Projects/OpenSource/cpp-anything/tests/fixtures/ts/cpp/textkit/build"],
      "defines": ["NODE_GYP_MODULE_NAME=textkit_shadow"]
    }
  ]
}
