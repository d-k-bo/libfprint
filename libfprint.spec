Name:           libfprint

Version:        1.94.4.goodixtls.{{{ git_dir_version }}}
Release:        1
Summary:        Toolkit for fingerprint scanner

License:        LGPLv2+
URL:            https://github.com/d-k-bo/libfprint/tree/55b4-experimental

VCS:            {{{ git_dir_vcs }}}
Source:        	{{{ git_dir_pack }}}

ExcludeArch:    s390 s390x

BuildRequires:  meson
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  git
BuildRequires:  pkgconfig(glib-2.0) >= 2.50
BuildRequires:  pkgconfig(gio-2.0) >= 2.44.0
BuildRequires:  pkgconfig(gusb) >= 0.3.0
BuildRequires:  pkgconfig(nss)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  gtk-doc
BuildRequires:  libgudev-devel
# For the udev.pc to install the rules
BuildRequires:  systemd
BuildRequires:  gobject-introspection-devel
# For internal CI tests; umockdev 0.13.2 has an important locking fix
BuildRequires:  python3-cairo python3-gobject cairo-devel
BuildRequires:  umockdev >= 0.13.2
# For the sigfm algorithm
BuildRequires:  pkgconfig(opencv4)
BuildRequires:  cmake
BuildRequires:  doctest-devel
# For goodixtls devices
BuildRequires:  pkgconfig(openssl)

%description
libfprint offers support for consumer fingerprint reader devices. This version offers experimental support for some sensors manufactured by Goodix.

%package        devel
Summary:        Development files for %{name}
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description    devel
The %{name}-devel package contains libraries and header files for
developing applications that use %{name}.

%prep
{{{ git_dir_setup_macro }}}

%build
# Include the virtual image driver for integration tests
%meson -Ddrivers=all
%meson_build

%install
%meson_install

%ldconfig_scriptlets

%check
# libfprint tests are currently disabled
%dnl %meson_test -t 4

%files
%license COPYING
%doc NEWS THANKS AUTHORS README.md
%{_libdir}/*.so.*
%{_libdir}/girepository-1.0/*.typelib
%{_udevhwdbdir}/60-autosuspend-libfprint-2.hwdb
%{_udevrulesdir}/70-libfprint-2.rules

%files devel
%doc HACKING.md
%{_includedir}/*
%{_libdir}/*.so
%{_libdir}/pkgconfig/%{name}-2.pc
%{_datadir}/gir-1.0/*.gir
%{_datadir}/gtk-doc/html/libfprint-2/

%changelog
{{{ git_dir_changelog }}}
