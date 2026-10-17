Name:           t-code
Version:        0.1
Release:        1%{?dist}
Summary:        Terminal-centric C/C++ IDE: file manager, micro editor tabs and a shell
License:        MIT
URL:            https://github.com/suacayipkisi/t-code
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(vte-2.91-gtk4)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(webkitgtk-6.0)
BuildRequires:  pkgconfig(md4c-html)
BuildRequires:  desktop-file-utils

Requires:       micro
Requires:       clang-tools-extra
Requires:       git-core
Requires:       cascadia-code-fonts
Requires:       hicolor-icon-theme
Recommends:     ripgrep
Recommends:     (wl-clipboard or xclip)

%description
t-code is a lightweight C/C++ IDE where every pane is a real terminal: a file
manager (t-code-fm), editor tabs running micro with clangd, and a shell, in a
GTK4 window with VS Code Dark Modern colors. It also has a Search panel, Source
Control, git comparisons and Markdown previews.

%prep
%autosetup

%build
%make_build build/t-code build/t-code-fm build/t-code-lsp

%install
%make_install PREFIX=%{_prefix} BINDIR=%{_bindir} LIBEXECDIR=%{_libexecdir} DATADIR=%{_datadir}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.tcode.desktop
%{buildroot}%{_bindir}/t-code --version | grep -qx 't-code %{version}'
# The other tests open windows and need a display.
%make_build build/test-searchmatch
build/test-searchmatch

%files
%license LICENSE
%doc README.md
%{_bindir}/t-code
%{_libexecdir}/t-code/
%{_datadir}/t-code/
%{_datadir}/applications/io.github.tcode.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.tcode.svg

%changelog
* Sat Oct 17 2026 suacayipkisi <ufukdenizcaglayan@gmail.com> - 0.1-1
- First release
