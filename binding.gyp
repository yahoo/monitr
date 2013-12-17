{
  "targets": [
    {
      "target_name": "monitor",
      "sources": [ "src/monitor.cc" ],
      "cflags_cc" : ["-fexceptions"],
      "cflags": ["-fexceptions"],
      "conditions": [
        ["OS=='mac'", {
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES"
          }
        }]
      ]
    }
  ]
}
