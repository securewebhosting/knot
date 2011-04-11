/*!
 * \file zone-dump.h
 *
 * \author Jan Kadlec <jan.kadlec@nic.cz>
 *
 * \brief Functions for dumping zone to binary file.
 *
 * \addtogroup dnslib
 * @{
 */

#ifndef _KNOT_DNSLIB_ZONEDUMP_H_
#define _KNOT_DNSLIB_ZONEDUMP_H_

#include "dnslib/zone.h"

/*!
 * \brief Zone loader enums.
 */
enum {
	MAGIC_LENGTH = 7 /*!< Compiled zone magic length. */
};

/*! \brief Magic identifier: { "knot", maj_ver, min_ver, revision } */
#define MAGIC_BYTES {'k', 'n', 'o', 't', '0', '2', 'a'}

/*!
 * \brief Dumps given zone to binary file.
 *
 * \param zone Zone to be saved.
 * \param filename Name of file to be created.
 * \param do_checks Set to 1 to enable checking the zone for semantic errors.
 * \param sfilename Source filename of the text zone file.
 *
 * \retval DNSLIB_EOK on success.
 * \retval DNSLIB_EBADARG if the file cannot be opened for writing.
 */
int dnslib_zdump_binary(dnslib_zone_t *zone, const char *filename,
			char do_checks, const char *sfilename);

#endif /* _DNSLIB_ZONEDUMP_H_ */

/*! @} */
