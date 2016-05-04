Name:		growl-for-linux
Provides:	gol
Version:	0.8.4
Release:	1%{?dist}
Summary:	Linux compatible Growl
Group:		GNOME Desktop
License:	BSD style license
URL:		http://mattn.github.com/growl-for-linux
Source0:	https://github.com/mattn/growl-for-linux/archive/growl-for-linux-%{version}.tar.gz

BuildRequires:	autoconf libtool glib2-devel gtk2-devel dbus-glib-devel libxml2-devel libcurl-devel sqlite-devel libnotify-devel libappindicator-devel openssl-devel
Requires:	glib2 gtk2 dbus-glib libxml2 libcurl sqlite

%description
Growl For Linux is Linux-compatible Growl. Growl is a notification system for Mac OS X.


%prep
%setup -q
./autogen.sh


%build
%configure
make %{?_smp_mflags}


%install
make install DESTDIR=%{buildroot}


%files
%defattr(-,root,root,-)
%doc NEWS README README.mkd TODO
%{_bindir}/gol
%{_datadir}/growl-for-linux/data/*
%{_datadir}/applications/gol.desktop
%dir %{_libdir}/growl-for-linux/display
%dir %{_libdir}/growl-for-linux/subscribe
%{_libdir}/growl-for-linux/display/libballoon.so*
%{_libdir}/growl-for-linux/display/libfog.so*
%{_libdir}/growl-for-linux/display/libnico2.so*

%exclude %{_libdir}/growl-for-linux/display/*.a
%exclude %{_libdir}/growl-for-linux/display/*.la
%exclude %{_libdir}/growl-for-linux/subscribe/*.a
%exclude %{_libdir}/growl-for-linux/subscribe/*.la
%exclude %{_libdir}/growl-for-linux/subscribe/libtweets.so*


%package display-notify
Summary:	Growl for Linux display plugin
Group:		GNOME Desktop
Requires:	%{name} libnotify

%description display-notify
Growl for Linux display plugin.


%files display-notify
%defattr(-,root,root,-)
%{_libdir}/growl-for-linux/display/libnotify_gol.so*


%package subscribe-rhythmbox
Summary:	Growl for Linux subscribe plugin
Group:		GNOME Desktop
Requires:	%{name}

%description subscribe-rhythmbox
Growl for Linux subscribe plugin


%files subscribe-rhythmbox
%defattr(-,root,root,-)
%{_libdir}/growl-for-linux/subscribe/librhythmbox.so*


%changelog
* Wed May 4 2016 Kohei Takahashi <flast@flast.jp> - 0.8.4-1
- Increase version number

* Sat May 10 2014 Kohei Takahashi <flast@flast.jp> - 0.7.9-2
- Fix unexpected plugin packaging

* Sat May 10 2014 Kohei Takahashi <flast@flast.jp> - 0.7.9-1
- First release for RPM package
