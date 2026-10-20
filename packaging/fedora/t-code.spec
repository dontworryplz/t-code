Name:           t-code
Version:        0.2
Release:        1%{?dist}
Summary:        Lightweight C/C++ IDE: file tree, editor tabs with clangd and a shell
License:        MIT
URL:            https://github.com/suacayipkisi/t-code
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(gtksourceview-5)
BuildRequires:  pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(vte-2.91-gtk4)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(md4c)
BuildRequires:  desktop-file-utils

Requires:       clang-tools-extra
Requires:       git-core
Requires:       cascadia-code-fonts
Requires:       hicolor-icon-theme
Recommends:     ripgrep

%description
t-code is a lightweight C/C++ IDE: a file tree,
GtkSourceView editor tabs sharing one clangd (errors underlined, completion,
parameter hints, go to definition) and a shell, in a GTK4 window with VS Code
Dark Modern colors. It also has a Search panel, Source Control, git comparisons
and Markdown previews.

%prep
%autosetup

%build
%make_build build/t-code

%install
%make_install PREFIX=%{_prefix} BINDIR=%{_bindir} DATADIR=%{_datadir}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.tcode.desktop
%{buildroot}%{_bindir}/t-code --version | grep -qx 't-code %{version}'
# The other tests open windows and need a display.
%make_build build/test-searchmatch build/test-detect build/test-linediff
build/test-searchmatch
build/test-detect
build/test-linediff

%files
%license LICENSE
%doc README.md
%{_bindir}/t-code
%{_datadir}/t-code/
%{_datadir}/applications/io.github.tcode.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.tcode.svg

%changelog
* Mon Oct 19 2026 suacayipkisi <ufukdenizcaglayan@gmail.com> - 0.2-1
- Editor tabs are GtkSourceView 5 views instead of micro terminals, with an
  in-process clangd client: problems underlined, completion, parameter hints,
  hover, go to definition, references, rename, formatting, semantic colors
- Find and replace bar, closing brackets and quotes, line commands, indent
  guides, a minimap marking problems, git's changes in the gutter
- Side-by-side comparisons stay aligned and scroll together
- Files, Source Control, Search and the shell can be moved between 12 docks
- Native Markdown preview (md4c in a text view): no WebKit
- Less memory: one clangd with at most five open documents, freed heap
  returned to the system, no process per editor
* Sat Oct 17 2026 suacayipkisi <ufukdenizcaglayan@gmail.com> - 0.1.1-1
- Much less memory and CPU: editors start when their tab is first shown, hidden
  editors stay idle, clangd runs with two malloc arenas, the Markdown preview's
  network process exits with the last preview
- Opening t-code no longer flashes clipboard windows or clears the clipboard
- Tests for the shared language servers, including a heap growth check

* Sat Oct 17 2026 suacayipkisi <ufukdenizcaglayan@gmail.com> - 0.1-1
- First release
