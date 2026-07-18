{
  "variables": {
    "caeneus_root": "<(module_root_dir)/../..",
    "caeneus_include_dir": "<(caeneus_root)/include",
    "caeneus_lib_dir": "<(caeneus_root)/zig-out/lib",
    "caeneus_target_name": "caeneus",
    "caeneus_shared": 0
  },
  "targets": [
    {
      "target_name": "<(caeneus_target_name)",
      "include_dirs": ["<(caeneus_include_dir)"],
      "defines": ["NAPI_VERSION=8"],
      "conditions": [
        [
          "caeneus_target_name=='caeneus'",
          {
            "sources": ["binding_fast.cpp"],
            "cflags_cc": ["-std=c++20"]
          }
        ],
        [
          "caeneus_target_name=='caeneus_bun'",
          {
            "sources": ["binding.cpp"],
            "cflags_cc": ["-std=c++17"]
          }
        ],
        [
          "OS=='linux' and caeneus_shared==0",
          {
            "libraries": [
              "<(caeneus_lib_dir)/libcaeneus.a",
              "-ldl",
              "-lpthread",
              "-lm"
            ]
          }
        ],
        [
          "OS=='linux' and caeneus_shared==1",
          {
            "libraries": ["<(caeneus_lib_dir)/libcaeneus.so", "-ldl", "-lpthread", "-lm"],
            "ldflags": ["-Wl,-rpath,<(caeneus_lib_dir)"]
          }
        ],
        [
          "OS=='mac' and caeneus_shared==0",
          {
            "libraries": [
              "<(caeneus_lib_dir)/libcaeneus.a",
              "-lpthread"
            ]
          }
        ],
        [
          "OS=='mac' and caeneus_shared==1",
          {
            "libraries": ["<(caeneus_lib_dir)/libcaeneus.dylib", "-lpthread"],
            "ldflags": ["-Wl,-rpath,<(caeneus_lib_dir)"]
          }
        ],
        [
          "OS=='win'",
          {
            "libraries": ["<(caeneus_lib_dir)/caeneus-static.lib", "ntdll.lib"],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "LanguageStandard": "stdcpp20"
              }
            }
          }
        ]
      ]
    }
  ]
}
