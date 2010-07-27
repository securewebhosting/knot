/*!
 * @file dynamic-array.h
 *
 * @todo Somehow check if the array is initialized and do not use otherwise.
 *       Maybe some magic, or so.
 */
#ifndef DYNAMIC_ARRAY
#define DYNAMIC_ARRAY

#include <string.h>
#include <pthread.h>
#include "common.h"

/*----------------------------------------------------------------------------*/
/*!
 * @brief Dynamic array structure.
 *
 * Before using the dynamic array, it must be initialized using da_initialize().
 * When getting individual items always use da_get_items() to obtain pointer to
 * the actual array.
 *
 * Items in the array cannot be dereferenced (it uses void * for storing the
 * the items). It is needed to type-cast the item array (obtained by calling
 * da_get_items()) to a proper type before dereferencing.
 *
 * When adding items, first reserve enough space for them by callling
 * da_reserve() and subsequently tell the array about the inserted items by
 * calling da_occupy(). When removing, the array must be told about the fact
 * by calling da_release().
 *
 * For getting the actual number of items in array use da_get_count().
 *
 * When the array is no more needed, the da_destroy() function must be called
 * before deallocating the structure.
 */
typedef struct {
	/*! @brief The actual array. The items cannot be directly dereferenced. */
	void *items;

	/*!
	 * @brief Size of the stored items in bytes (used in counting of space
	 *        needed.
	 */
	size_t item_size;

	/*! @brief Size of allocated space in number of items that can be stored. */
	uint allocated;

	/*! @brief Number of items actually stored in the array. */
	uint count;

	/*! @brief  */
	pthread_mutex_t mtx;
} da_array;

/*----------------------------------------------------------------------------*/
/*!
 * @brief Initializes the dynamic array by allocating place for @a count items
 *        of size @a item_size and setting the items to zeros.
 *
 * @retval 0 if successful.
 * @retval -1 if not successful.
 */
int da_initialize( da_array *array, uint count, size_t item_size );

/*!
 * @brief Reserves space for @a count more items.
 *
 * @retval 0 if successful and resizing was not necessary.
 * @retval 1 if successful and the array was enlarged.
 * @retval -1 if not successful - resizing was needed but could not be done.
 */
int da_reserve( da_array *array, uint count );

/*!
 * @brief Increases the number of items in array by @a count.
 *
 * @retval 0 If successful.
 * @retval -1 If not successful (not enough allocated space, i.e. must run
 *            da_reserve()).
 */
int da_occupy( da_array *array, uint count );

/*!
 * @brief Tries to reserve space for @a count more items.
 *
 * @retval 0 if successful and resizing is not necessary.
 * @retval 1 if successful but the array will need to be resized.
 */
uint da_try_reserve( const da_array *array, uint count );

/*!
 * @brief Releases space taken by @a count items.
 */
void da_release( da_array *array, uint count );

/*!
 * @brief Poperly deallocates the array.
 */
void da_destroy( da_array *array );

/*!
 * @brief Returns the array of items as a void *.
 */
void *da_get_items( const da_array *array );

/*!
 * @brief Returns count of items in the array.
 */
uint da_get_count( const da_array *array );

/*----------------------------------------------------------------------------*/

#endif	// DYNAMIC_ARRAY
