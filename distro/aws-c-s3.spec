Name:           aws-c-s3
Version:        0.1.27
Release:        5%{?dist}
Summary:        C99 library implementation for communicating with the S3 service

License:        ASL 2.0
URL:            https://github.com/awslabs/%{name}
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz
Patch0:         aws-c-s3-cmake.patch

BuildRequires:  gcc
BuildRequires:  cmake
BuildRequires:  aws-c-common-devel
BuildRequires:  aws-c-sdkutils-devel
BuildRequires:  aws-c-cal-devel
BuildRequires:  aws-c-compression-devel
BuildRequires:  aws-c-io-devel
BuildRequires:  aws-c-http-devel
BuildRequires:  aws-c-auth-devel
BuildRequires:  aws-checksums-devel

Requires:       aws-c-common-libs
Requires:       aws-c-sdkutils-libs
Requires:       aws-c-cal-libs
Requires:       aws-c-compression-libs
Requires:       aws-c-io-libs
Requires:       aws-c-http-libs
Requires:       aws-c-auth-libs
Requires:       aws-checksums-libs

%description
C99 library implementation for communicating with the S3 service,
designed for maximizing throughput on high bandwidth EC2 instances.

%package libs
Summary:        C99 library implementation for communicating with the S3 service

%description libs
C99 library implementation for communicating with the S3 service,
designed for maximizing throughput on high bandwidth EC2 instances.


%package devel
Summary:        C99 library implementation for communicating with the S3 service
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}

%description devel
C99 library implementation for communicating with the S3 service,
designed for maximizing throughput on high bandwidth EC2 instances.


%prep
%autosetup -p1


%build
%cmake -DBUILD_SHARED_LIBS=ON
%cmake_build

%install
%cmake_install

%files
%{_bindir}/s3

%files libs
%license LICENSE
%doc README.md
%{_libdir}/libaws-c-s3.so.0unstable
%{_libdir}/libaws-c-s3.so.1.0.0

%files devel
%dir %{_includedir}/aws/s3
%{_includedir}/aws/s3/*.h

%dir %{_libdir}/cmake/aws-c-s3
%dir %{_libdir}/cmake/aws-c-s3/shared
%{_libdir}/libaws-c-s3.so
%{_libdir}/cmake/aws-c-s3/aws-c-s3-config.cmake
%{_libdir}/cmake/aws-c-s3/shared/aws-c-s3-targets-noconfig.cmake
%{_libdir}/cmake/aws-c-s3/shared/aws-c-s3-targets.cmake



%changelog
* Tue Feb 22 2022 David Duncan <davdunc@amazon.com> - 0.1.27-5
- Updated for package review

* Tue Feb 22 2022 Kyle Knapp <kyleknap@amazon.com> - 0.1.27-4
- Include missing devel directories

* Thu Feb 03 2022 Kyle Knapp <kyleknap@amazon.com> - 0.1.27-3
- Update specfile based on review feedback

* Wed Feb 02 2022 David Duncan <davdunc@amazon.com> - 0.1.27-2
- Prepare for package review

* Tue Jan 18 2022 Kyle Knapp <kyleknap@amazon.com> - 0.1.27-1
- Initial package development
