{
  "targets": [
    {
      "target_name": "monitor",
      "sources": [ "src/monitor.cc" ],
      "cflags_cc" : ["-fexceptions"],
      "include_dirs" : [
          "<(node_root_dir)/deps/cares/include",
          "<!(node -e \"require('nan')\")"
      ]
    }
  ]
}
