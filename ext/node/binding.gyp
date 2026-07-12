{
  "variables": {
    "caeneus_root": "<(module_root_dir)/../..",
    "caeneus_include_dir": "<(caeneus_root)/include",
    "caeneus_lib_dir": "<(caeneus_root)/zig-out/lib",
    "caeneus_target_name": "caeneus",
    "caeneus_fast_api": 0,
    "caeneus_shared": 0
  },
  "targets": [
    {
      "target_name": "<(caeneus_target_name)",
      "sources": ["binding.cpp"],
      "include_dirs": ["<(caeneus_include_dir)"],
      "defines": ["NAPI_VERSION=8"],
      "conditions": [
        [
          "OS=='linux' and caeneus_shared==0",
          {
            "libraries": [
              "<(caeneus_lib_dir)/libcaeneus.a",
              "-ldl",
              "-lpthread",
              "-lm"
            ],
            "cflags_cc": ["-std=c++17"]
          }
        ],
        [
          "OS=='linux' and caeneus_shared==1",
          {
            "libraries": ["<(caeneus_lib_dir)/libcaeneus.so", "-ldl", "-lpthread", "-lm"],
            "ldflags": ["-Wl,-rpath,<(caeneus_lib_dir)"],
            "cflags_cc": ["-std=c++17"]
          }
        ],
        [
          "OS=='mac' and caeneus_shared==0",
          {
            "libraries": [
              "<(caeneus_lib_dir)/libcaeneus.a",
              "-lpthread"
            ],
            "cflags_cc": ["-std=c++17"]
          }
        ],
        [
          "OS=='mac' and caeneus_shared==1",
          {
            "libraries": ["<(caeneus_lib_dir)/libcaeneus.dylib", "-lpthread"],
            "ldflags": ["-Wl,-rpath,<(caeneus_lib_dir)"],
            "cflags_cc": ["-std=c++17"]
          }
        ],
        [
          "OS=='win'",
          {
            "libraries": ["<(caeneus_lib_dir)/caeneus-static.lib", "ntdll.lib"],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "LanguageStandard": "stdcpp17"
              }
            }
          }
        ],
        [
          "caeneus_fast_api==1",
          {
            "defines": ["CAENEUS_ENABLE_V8_FAST_API=1"],
            "conditions": [
              [
                "OS!='win'",
                {
                  "cflags_cc": ["-std=c++17"]
                }
              ]
            ]
          }
        ]
      ]
    }
  ]
}
