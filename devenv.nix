{
  pkgs,
  ...
}:

{
  packages = [
    # tools
    pkgs.cmake
    pkgs.gnumake
    pkgs.ninja
    pkgs.bear
    pkgs.b3sum
    pkgs.jq
    pkgs.perf-tools
    pkgs.perf

    # libs
    pkgs.libbpf
    pkgs.liburing
    pkgs.jemalloc

    # needed to compile bpf programs
    pkgs.linuxHeaders
    pkgs.llvmPackages.clang-unwrapped # wrapped uses switches incompatible with bpf
  ];

  languages.c.enable = true;
  languages.rust.enable = true;

  env = {
    LIBBPF = "${pkgs.libbpf}/include";
    LINUX = "${pkgs.linuxHeaders}/include";
  };
}
