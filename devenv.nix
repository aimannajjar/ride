{
  pkgs,
  ...
}:

{
  packages = [
    pkgs.cmake
    pkgs.gnumake
    pkgs.ninja
    pkgs.bear
    pkgs.b3sum

    # needed to compile bpf programs
    pkgs.libbpf
    pkgs.linuxHeaders
    pkgs.llvmPackages.clang-unwrapped # wrapped uses switches incompatible with bpf

  ];

  languages.c.enable = true;

  env = {
    LIBBPF = "${pkgs.libbpf}/include";
    LINUX = "${pkgs.linuxHeaders}/include";
  };
}
