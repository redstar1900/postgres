/*-------------------------------------------------------------------------
 *
 * tupmacs.h
 *	  Tuple macros used by both index tuples and heap tuples.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupmacs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPMACS_H
#define TUPMACS_H

#include "catalog/pg_type_d.h"	/* for TYPALIGN macros */


/*
 * Check a tuple's null bitmap to determine whether the attribute is null.
 * Note that a 0 in the null bitmap indicates a null, while 1 indicates
 * non-null.
 */
static inline bool
att_isnull(int ATT, const bits8 *BITS)
{
	return !(BITS[ATT >> 3] & (1 << (ATT & 0x07)));
}

#ifndef FRONTEND
/*
 * Given an attbyval and an attlen from either a Form_pg_attribute or
 * CompactAttribute and a pointer into a tuple's data area, return the
 * correct value or pointer.
 *
 * We return a Datum value in all cases.  If attbyval is false,  we return the
 * same pointer into the tuple data area that we're passed.  Otherwise, we
 * return the correct number of bytes fetched from the data area and extended
 * to Datum form.
 *
 * On machines where Datum is 8 bytes, we support fetching 8-byte byval
 * attributes; otherwise, only 1, 2, and 4-byte values are supported.
 *
 * Note that T must already be properly aligned for this to work correctly.
 */
#define fetchatt(A,T) fetch_att(T, (A)->attbyval, (A)->attlen)

/*
 * Same, but work from byval/len parameters rather than Form_pg_attribute.
 */
static inline Datum
fetch_att(const void *T, bool attbyval, int attlen)
{
	if (attbyval)
	{
		switch (attlen)
		{
			case sizeof(char):
				return CharGetDatum(*((const char *) T));
			case sizeof(int16):
				return Int16GetDatum(*((const int16 *) T));
			case sizeof(int32):
				return Int32GetDatum(*((const int32 *) T));
#if SIZEOF_DATUM == 8
			case sizeof(Datum):
				return *((const Datum *) T);
#endif
			default:
				elog(ERROR, "unsupported byval length: %d", attlen);
				return 0;
		}
	}
	else
		return PointerGetDatum(T);
}
#endif							/* FRONTEND */

/*
 * att_align_datum aligns the given offset as needed for a datum of alignment
 * requirement attalign and typlen attlen.  attdatum is the Datum variable
 * we intend to pack into a tuple (it's only accessed if we are dealing with
 * a varlena type).  Note that this assumes the Datum will be stored as-is;
 * callers that are intending to convert non-short varlena datums to short
 * format have to account for that themselves.
 */
#define att_align_datum(cur_offset, attalign, attlen, attdatum) \
( \
	((attlen) == -1 && VARATT_IS_SHORT(DatumGetPointer(attdatum))) ? \
	(uintptr_t) (cur_offset) : \
	att_align_nominal(cur_offset, attalign) \
)

/*
 * Similar to att_align_datum, but accepts a number of bytes, typically from
 * CompactAttribute.attalignby to align the Datum by.
 */
#define att_datum_alignby(cur_offset, attalignby, attlen, attdatum) \
	( \
	((attlen) == -1 && VARATT_IS_SHORT(DatumGetPointer(attdatum))) ? \
	(uintptr_t) (cur_offset) : \
	TYPEALIGN(attalignby, cur_offset))

/*
 * att_align_pointer performs the same calculation as att_align_datum,
 * but is used when walking a tuple.  attptr is the current actual data
 * pointer; when accessing a varlena field we have to "peek" to see if we
 * are looking at a pad byte or the first byte of a 1-byte-header datum.
 * (A zero byte must be either a pad byte, or the first byte of a correctly
 * aligned 4-byte length word; in either case we can align safely.  A non-zero
 * byte must be either a 1-byte length word, or the first byte of a correctly
 * aligned 4-byte length word; in either case we need not align.)
 *
 * Note: some callers pass a "char *" pointer for cur_offset.  This is
 * a bit of a hack but should work all right as long as uintptr_t is the
 * correct width.
 */
#define att_align_pointer(cur_offset, attalign, attlen, attptr) \
( \
	((attlen) == -1 && VARATT_NOT_PAD_BYTE(attptr)) ? \
	(uintptr_t) (cur_offset) : \
	att_align_nominal(cur_offset, attalign) \
)

/*
 * Similar to att_align_pointer, but accepts a number of bytes, typically from
 * CompactAttribute.attalignby to align the pointer by.
 */
#define att_pointer_alignby(cur_offset, attalignby, attlen, attptr) \
	( \
	((attlen) == -1 && VARATT_NOT_PAD_BYTE(attptr)) ? \
	(uintptr_t) (cur_offset) : \
	TYPEALIGN(attalignby, cur_offset))

/*
 * att_align_nominal aligns the given offset as needed for a datum of alignment
 * requirement attalign, ignoring any consideration of packed varlena datums.
 * There are three main use cases for using this macro directly:
 *	* we know that the att in question is not varlena (attlen != -1);
 *	  in this case it is cheaper than the above macros and just as good.
 *	* we need to estimate alignment padding cost abstractly, ie without
 *	  reference to a real tuple.  We must assume the worst case that
 *	  all varlenas are aligned.
 *	* within arrays and multiranges, we unconditionally align varlenas (XXX this
 *	  should be revisited, probably).
 *
 * The attalign cases are tested in what is hopefully something like their
 * frequency of occurrence.
 */
#define att_align_nominal(cur_offset, attalign) \
( \
	((attalign) == TYPALIGN_INT) ? INTALIGN(cur_offset) : \
	 (((attalign) == TYPALIGN_CHAR) ? (uintptr_t) (cur_offset) : \
	  (((attalign) == TYPALIGN_DOUBLE) ? DOUBLEALIGN(cur_offset) : \
	   (((attalign) == TYPALIGN_XID) ? MAXALIGN(cur_offset) : \
	   ( \
			AssertMacro((attalign) == TYPALIGN_SHORT), \
			SHORTALIGN(cur_offset) \
	   )))) \
)

/*
 * Similar to att_align_nominal, but accepts a number of bytes, typically from
 * CompactAttribute.attalignby to align the offset by.
 */
#define att_nominal_alignby(cur_offset, attalignby) \
	TYPEALIGN(attalignby, cur_offset)

/*
 * att_addlength_datum increments the given offset by the space needed for
 * the given Datum variable.  attdatum is only accessed if we are dealing
 * with a variable-length attribute.
 */
#define att_addlength_datum(cur_offset, attlen, attdatum) \
	att_addlength_pointer(cur_offset, attlen, DatumGetPointer(attdatum))

/*
 * att_addlength_pointer performs the same calculation as att_addlength_datum,
 * but is used when walking a tuple --- attptr is the pointer to the field
 * within the tuple.
 *
 * Note: some callers pass a "char *" pointer for cur_offset.  This is
 * actually perfectly OK, but probably should be cleaned up along with
 * the same practice for att_align_pointer.
 */
#define att_addlength_pointer(cur_offset, attlen, attptr) \
( \
	((attlen) > 0) ? \
	( \
		(cur_offset) + (attlen) \
	) \
	: (((attlen) == -1) ? \
	( \
		(cur_offset) + VARSIZE_ANY(attptr) \
	) \
	: \
	( \
		AssertMacro((attlen) == -2), \
		(cur_offset) + (strlen((char *) (attptr)) + 1) \
	)) \
)

#ifndef FRONTEND
/*
 * store_att_byval is a partial inverse of fetch_att: store a given Datum
 * value into a tuple data area at the specified address.  However, it only
 * handles the byval case, because in typical usage the caller needs to
 * distinguish by-val and by-ref cases anyway, and so a do-it-all function
 * wouldn't be convenient.
 */
static inline void
store_att_byval(void *T, Datum newdatum, int attlen)
{
	switch (attlen)
	{
		case sizeof(char):
			*(char *) T = DatumGetChar(newdatum);
			break;
		case sizeof(int16):
			*(int16 *) T = DatumGetInt16(newdatum);
			break;
		case sizeof(int32):
			*(int32 *) T = DatumGetInt32(newdatum);
			break;
#if SIZEOF_DATUM == 8
		case sizeof(Datum):
			*(Datum *) T = newdatum;
			break;
#endif
		default:
			elog(ERROR, "unsupported byval length: %d", attlen);
	}
}
#endif							/* FRONTEND */

#endif							/* TUPMACS_H */
