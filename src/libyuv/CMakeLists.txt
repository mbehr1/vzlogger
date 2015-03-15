# -*- mode: cmake; -*-

set (libyuv_src
	planar_functions.cc
	convert_argb.cc
	row_common.cc
	)

add_library (libyuv ${libyuv_src})

