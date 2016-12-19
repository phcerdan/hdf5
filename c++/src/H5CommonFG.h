// C++ informative line for the emacs editor: -*- C++ -*-
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the files COPYING and Copyright.html.  COPYING can be found at the root   *
 * of the source code distribution tree; Copyright.html can be found at the  *
 * root level of an installed copy of the electronic HDF5 document set and   *
 * is linked from the top-level documents page.  It can also be found at     *
 * http://hdfgroup.org/HDF5/doc/Copyright.html.  If you do not have          *
 * access to either file, you may request a copy from help@hdfgroup.org.     *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef __CommonFG_H
#define __CommonFG_H

namespace H5 {

// Class forwarding
class Group;
class H5File;
class ArrayType;
class VarLenType;

/*! \class CommonFG
    \brief \a CommonFG is an abstract base class of H5File and H5Group.

    It provides common operations of H5File and H5Group.
*/
class H5_DLLCPP CommonFG {
   public:
	// Opens a generic named datatype in this location.
	DataType openDataType(const char* name) const;
	DataType openDataType(const H5std_string& name) const;

	// Opens a named array datatype in this location.
	ArrayType openArrayType(const char* name) const;
	ArrayType openArrayType(const H5std_string& name) const;

	// Opens a named compound datatype in this location.
	CompType openCompType(const char* name) const;
	CompType openCompType(const H5std_string& name) const;

	// Opens a named enumeration datatype in this location.
	EnumType openEnumType(const char* name) const;
	EnumType openEnumType(const H5std_string& name) const;

	// Opens a named integer datatype in this location.
	IntType openIntType(const char* name) const;
	IntType openIntType(const H5std_string& name) const;

	// Opens a named floating-point datatype in this location.
	FloatType openFloatType(const char* name) const;
	FloatType openFloatType(const H5std_string& name) const;

	// Opens a named string datatype in this location.
	StrType openStrType(const char* name) const;
	StrType openStrType(const H5std_string& name) const;

	// Opens a named variable length datatype in this location.
	VarLenType openVarLenType(const char* name) const;
	VarLenType openVarLenType(const H5std_string& name) const;

#ifndef DOXYGEN_SHOULD_SKIP_THIS
	/// For subclasses, H5File and Group, to return the correct
	/// object id, i.e. file or group id.
	virtual hid_t getLocId() const = 0;


	/// For subclasses, H5File and Group, to throw appropriate exception.
	virtual void throwException(const H5std_string& func_name, const H5std_string& msg) const = 0;

	// Default constructor.
	CommonFG();

	// Noop destructor.
	virtual ~CommonFG();

    protected:
	virtual void p_setId(const hid_t new_id) = 0;

#endif // DOXYGEN_SHOULD_SKIP_THIS

}; // end of CommonFG declaration

#ifndef H5_NO_NAMESPACE
}
#endif
#endif // __CommonFG_H

