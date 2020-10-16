/*
 *  PSA crypto layer on top of Mbed TLS crypto
 */
/*
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "common.h"

#if defined(MBEDTLS_PSA_CRYPTO_C)

#include "psa_crypto_service_integration.h"
#include "psa/crypto.h"

#include "psa_crypto_core.h"
#include "psa_crypto_slot_management.h"
#include "psa_crypto_storage.h"
#if defined(MBEDTLS_PSA_CRYPTO_SE_C)
#include "psa_crypto_se.h"
#endif

#include <stdlib.h>
#include <string.h>
#if defined(MBEDTLS_PLATFORM_C)
#include "mbedtls/platform.h"
#else
#define mbedtls_calloc calloc
#define mbedtls_free   free
#endif

#define ARRAY_LENGTH( array ) ( sizeof( array ) / sizeof( *( array ) ) )

typedef struct
{
    psa_key_slot_t key_slots[PSA_KEY_SLOT_COUNT];
    unsigned key_slots_initialized : 1;
} psa_global_data_t;

static psa_global_data_t global_data;

psa_status_t psa_validate_key_id(
    mbedtls_svc_key_id_t key, int vendor_ok, int volatile_ok )
{
    psa_key_id_t key_id = MBEDTLS_SVC_KEY_ID_GET_KEY_ID( key );

    if( ( PSA_KEY_ID_USER_MIN <= key_id ) &&
        ( key_id <= PSA_KEY_ID_USER_MAX ) )
        return( PSA_SUCCESS );

    if( vendor_ok &&
        ( PSA_KEY_ID_VENDOR_MIN <= key_id ) &&
        ( key_id < PSA_KEY_ID_VOLATILE_MIN ) )
        return( PSA_SUCCESS );

    if( volatile_ok &&
        ( PSA_KEY_ID_VOLATILE_MIN <= key_id ) &&
        ( key_id <= PSA_KEY_ID_VOLATILE_MAX ) )
        return( PSA_SUCCESS );

    return( PSA_ERROR_INVALID_HANDLE );
}

/** Search for the description of a key given its identifier.
 *
 *  The descriptions of volatile keys and loaded persistent keys are
 *  stored in key slots. This function returns a pointer to the key slot
 *  containing the description of a key given its identifier.
 *
 *  The function searches the key slots containing the description of the key
 *  with \p key identifier. The function does only read accesses to the key
 *  slots. The function does not load any persistent key thus does not access
 *  any storage.
 *
 *  For volatile key identifiers, only one key slot is queried as a volatile
 *  key with identifier key_id can only be stored in slot of index
 *  ( key_id - PSA_KEY_ID_VOLATILE_MIN ).
 *
 * \param key           Key identifier to query.
 * \param[out] p_slot   On success, `*p_slot` contains a pointer to the
 *                      key slot containing the description of the key
 *                      identified by \p key.
 *
 * \retval PSA_SUCCESS
 *         The pointer to the key slot containing the description of the key
 *         identified by \p key was returned.
 * \retval PSA_ERROR_INVALID_HANDLE
 *         \p key is not a valid key identifier.
 * \retval #PSA_ERROR_DOES_NOT_EXIST
 *         There is no key with key identifier \p key in the key slots.
 */
static psa_status_t psa_search_key_in_slots(
    mbedtls_svc_key_id_t key, psa_key_slot_t **p_slot )
{
    psa_key_id_t key_id = MBEDTLS_SVC_KEY_ID_GET_KEY_ID( key );
    psa_key_slot_t *slot = NULL;

    psa_status_t status = psa_validate_key_id( key, 1, 1 );
    if( status != PSA_SUCCESS )
        return( status );

    if( psa_key_id_is_volatile( key_id ) )
    {
        slot = &global_data.key_slots[ key_id - PSA_KEY_ID_VOLATILE_MIN ];

        if( ! mbedtls_svc_key_id_equal( key, slot->attr.id ) )
             status = PSA_ERROR_DOES_NOT_EXIST;
    }
    else
    {
        status = PSA_ERROR_DOES_NOT_EXIST;
        slot = &global_data.key_slots[ PSA_KEY_SLOT_COUNT ];

        while( slot > &global_data.key_slots[ 0 ] )
        {
            slot--;
            if( mbedtls_svc_key_id_equal( key, slot->attr.id ) )
            {
                status = PSA_SUCCESS;
                break;
            }
        }
    }

    if( status == PSA_SUCCESS )
        *p_slot = slot;

    return( status );
}

psa_status_t psa_initialize_key_slots( void )
{
    /* Nothing to do: program startup and psa_wipe_all_key_slots() both
     * guarantee that the key slots are initialized to all-zero, which
     * means that all the key slots are in a valid, empty state. */
    global_data.key_slots_initialized = 1;
    return( PSA_SUCCESS );
}

void psa_wipe_all_key_slots( void )
{
    size_t slot_idx;

    for( slot_idx = 0; slot_idx < PSA_KEY_SLOT_COUNT; slot_idx++ )
    {
        psa_key_slot_t *slot = &global_data.key_slots[ slot_idx ];
        (void) psa_wipe_key_slot( slot );
    }
    global_data.key_slots_initialized = 0;
}

psa_status_t psa_get_empty_key_slot( psa_key_id_t *volatile_key_id,
                                     psa_key_slot_t **p_slot )
{
    size_t slot_idx;

    if( ! global_data.key_slots_initialized )
        return( PSA_ERROR_BAD_STATE );

    for( slot_idx = PSA_KEY_SLOT_COUNT; slot_idx > 0; slot_idx-- )
    {
        *p_slot = &global_data.key_slots[ slot_idx - 1 ];
        if( ! psa_is_key_slot_occupied( *p_slot ) )
        {
            *volatile_key_id = PSA_KEY_ID_VOLATILE_MIN +
                               ( (psa_key_id_t)slot_idx ) - 1;

            return( PSA_SUCCESS );
        }
    }
    *p_slot = NULL;
    return( PSA_ERROR_INSUFFICIENT_MEMORY );
}

#if defined(MBEDTLS_PSA_CRYPTO_STORAGE_C)
static psa_status_t psa_load_persistent_key_into_slot( psa_key_slot_t *slot )
{
    psa_status_t status = PSA_SUCCESS;
    uint8_t *key_data = NULL;
    size_t key_data_length = 0;

    status = psa_load_persistent_key( &slot->attr,
                                      &key_data, &key_data_length );
    if( status != PSA_SUCCESS )
        goto exit;

#if defined(MBEDTLS_PSA_CRYPTO_SE_C)
    if( psa_key_lifetime_is_external( slot->attr.lifetime ) )
    {
        psa_se_key_data_storage_t *data;
        if( key_data_length != sizeof( *data ) )
        {
            status = PSA_ERROR_STORAGE_FAILURE;
            goto exit;
        }
        data = (psa_se_key_data_storage_t *) key_data;
        memcpy( &slot->data.se.slot_number, &data->slot_number,
                sizeof( slot->data.se.slot_number ) );
    }
    else
#endif /* MBEDTLS_PSA_CRYPTO_SE_C */
    {
        status = psa_copy_key_material_into_slot( slot, key_data, key_data_length );
        if( status != PSA_SUCCESS )
            goto exit;
    }

exit:
    psa_free_persistent_key_data( key_data, key_data_length );
    return( status );
}
#endif /* MBEDTLS_PSA_CRYPTO_STORAGE_C */

psa_status_t psa_get_key_slot( mbedtls_svc_key_id_t key,
                               psa_key_slot_t **p_slot )
{
    psa_status_t status = PSA_ERROR_CORRUPTION_DETECTED;

    *p_slot = NULL;
    if( ! global_data.key_slots_initialized )
        return( PSA_ERROR_BAD_STATE );

    status = psa_search_key_in_slots( key, p_slot );
    if( status != PSA_ERROR_DOES_NOT_EXIST )
        return( status );

#if defined(MBEDTLS_PSA_CRYPTO_STORAGE_C)
    psa_key_id_t volatile_key_id;

    status = psa_get_empty_key_slot( &volatile_key_id, p_slot );
    if( status != PSA_SUCCESS )
        return( status );

    (*p_slot)->attr.lifetime = PSA_KEY_LIFETIME_PERSISTENT;
    (*p_slot)->attr.id = key;

    status = psa_load_persistent_key_into_slot( *p_slot );
    if( status != PSA_SUCCESS )
        psa_wipe_key_slot( *p_slot );

    return( status );
#else
    return( PSA_ERROR_DOES_NOT_EXIST );
#endif /* defined(MBEDTLS_PSA_CRYPTO_STORAGE_C) */

}

psa_status_t psa_validate_key_location( psa_key_lifetime_t lifetime,
                                        psa_se_drv_table_entry_t **p_drv )
{
    if ( psa_key_lifetime_is_external( lifetime ) )
    {
#if defined(MBEDTLS_PSA_CRYPTO_SE_C)
        psa_se_drv_table_entry_t *driver = psa_get_se_driver_entry( lifetime );
        if( driver == NULL )
            return( PSA_ERROR_INVALID_ARGUMENT );
        else
        {
            if (p_drv != NULL)
                *p_drv = driver;
            return( PSA_SUCCESS );
        }
#else
        (void) p_drv;
        return( PSA_ERROR_INVALID_ARGUMENT );
#endif /* MBEDTLS_PSA_CRYPTO_SE_C */
    }
    else
        /* Local/internal keys are always valid */
        return( PSA_SUCCESS );
}

psa_status_t psa_validate_key_persistence( psa_key_lifetime_t lifetime )
{
    if ( PSA_KEY_LIFETIME_IS_VOLATILE( lifetime ) )
    {
        /* Volatile keys are always supported */
        return( PSA_SUCCESS );
    }
    else
    {
        /* Persistent keys require storage support */
#if defined(MBEDTLS_PSA_CRYPTO_STORAGE_C)
        return( PSA_SUCCESS );
#else /* MBEDTLS_PSA_CRYPTO_STORAGE_C */
        return( PSA_ERROR_NOT_SUPPORTED );
#endif /* !MBEDTLS_PSA_CRYPTO_STORAGE_C */
    }
}

psa_status_t psa_open_key( mbedtls_svc_key_id_t key, psa_key_handle_t *handle )
{
#if defined(MBEDTLS_PSA_CRYPTO_STORAGE_C)
    psa_status_t status;
    psa_key_slot_t *slot;

    status = psa_get_key_slot( key, &slot );
    if( status != PSA_SUCCESS )
    {
        *handle = PSA_KEY_HANDLE_INIT;
        return( status );
    }

    *handle = key;

    return( PSA_SUCCESS );

#else /* defined(MBEDTLS_PSA_CRYPTO_STORAGE_C) */
    (void) key;
    *handle = PSA_KEY_HANDLE_INIT;
    return( PSA_ERROR_NOT_SUPPORTED );
#endif /* !defined(MBEDTLS_PSA_CRYPTO_STORAGE_C) */
}

psa_status_t psa_close_key( psa_key_handle_t handle )
{
    psa_status_t status;
    psa_key_slot_t *slot;

    if( psa_key_handle_is_null( handle ) )
        return( PSA_SUCCESS );

    status = psa_search_key_in_slots( handle, &slot );
    if( status != PSA_SUCCESS )
        return( status );

    return( psa_wipe_key_slot( slot ) );
}

psa_status_t psa_purge_key( mbedtls_svc_key_id_t key )
{
    psa_status_t status;
    psa_key_slot_t *slot;

    status = psa_search_key_in_slots( key, &slot );
    if( status != PSA_SUCCESS )
        return( status );

    if( slot->attr.lifetime == PSA_KEY_LIFETIME_VOLATILE )
        return PSA_SUCCESS;

    return( psa_wipe_key_slot( slot ) );
}

void mbedtls_psa_get_stats( mbedtls_psa_stats_t *stats )
{
    size_t slot_idx;

    memset( stats, 0, sizeof( *stats ) );

    for( slot_idx = 0; slot_idx < PSA_KEY_SLOT_COUNT; slot_idx++ )
    {
        const psa_key_slot_t *slot = &global_data.key_slots[ slot_idx ];
        if( ! psa_is_key_slot_occupied( slot ) )
        {
            ++stats->empty_slots;
            continue;
        }
        if( slot->attr.lifetime == PSA_KEY_LIFETIME_VOLATILE )
            ++stats->volatile_slots;
        else if( slot->attr.lifetime == PSA_KEY_LIFETIME_PERSISTENT )
        {
            psa_key_id_t id = MBEDTLS_SVC_KEY_ID_GET_KEY_ID( slot->attr.id );
            ++stats->persistent_slots;
            if( id > stats->max_open_internal_key_id )
                stats->max_open_internal_key_id = id;
        }
        else
        {
            psa_key_id_t id = MBEDTLS_SVC_KEY_ID_GET_KEY_ID( slot->attr.id );
            ++stats->external_slots;
            if( id > stats->max_open_external_key_id )
                stats->max_open_external_key_id = id;
        }
    }
}

#endif /* MBEDTLS_PSA_CRYPTO_C */
