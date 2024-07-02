AC_DEFUN([AC_PACKAGE_NEED_URING_H],
  [ AC_CHECK_HEADERS(liburing.h, [ have_uring=true ], [ have_uring=false ])
    AC_SUBST(have_uring)
  ])
AC_DEFUN([AC_PACKAGE_NEED_LIBURING],
  [ AC_CHECK_LIB(uring, io_uring_submit,, [
	echo
	echo 'FATAL ERROR: could not find a valid liburing library.'
	echo 'Install the liburing library package.'
	exit 1
    ])
    liburing=-luring
    AC_SUBST(liburing)
  ])
