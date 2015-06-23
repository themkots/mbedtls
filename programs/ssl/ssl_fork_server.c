/*
 *  SSL server demonstration program using fork() for handling multiple clients
 *
 *  Copyright (C) 2006-2013, ARM Limited, All Rights Reserved
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#if !defined(MBEDTLS_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#if defined(MBEDTLS_PLATFORM_C)
#include "mbedtls/platform.h"
#else
#include <stdio.h>
#define mbedtls_fprintf    fprintf
#define mbedtls_printf     printf
#endif

#if !defined(MBEDTLS_BIGNUM_C) || !defined(MBEDTLS_CERTS_C) ||    \
    !defined(MBEDTLS_ENTROPY_C) || !defined(MBEDTLS_SSL_TLS_C) || \
    !defined(MBEDTLS_SSL_SRV_C) || !defined(MBEDTLS_NET_C) ||     \
    !defined(MBEDTLS_RSA_C) || !defined(MBEDTLS_CTR_DRBG_C) ||    \
    !defined(MBEDTLS_X509_CRT_PARSE_C) || !defined(MBEDTLS_TIMING_C) || \
    !defined(MBEDTLS_FS_IO) || !defined(MBEDTLS_PEM_PARSE_C)
int main( int argc, char *argv[] )
{
    ((void) argc);
    ((void) argv);

    mbedtls_printf("MBEDTLS_BIGNUM_C and/or MBEDTLS_CERTS_C and/or MBEDTLS_ENTROPY_C "
           "and/or MBEDTLS_SSL_TLS_C and/or MBEDTLS_SSL_SRV_C and/or "
           "MBEDTLS_NET_C and/or MBEDTLS_RSA_C and/or "
           "MBEDTLS_CTR_DRBG_C and/or MBEDTLS_X509_CRT_PARSE_C and/or "
           "MBEDTLS_TIMING_C and/or MBEDTLS_PEM_PARSE_C not defined.\n");
    return( 0 );
}
#elif defined(_WIN32)
int main( void )
{
    mbedtls_printf("_WIN32 defined. This application requires fork() and signals "
           "to work correctly.\n");
    return( 0 );
}
#else

#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/certs.h"
#include "mbedtls/x509.h"
#include "mbedtls/ssl.h"
#include "mbedtls/net.h"
#include "mbedtls/timing.h"

#include <string.h>
#include <signal.h>

#if !defined(_MSC_VER) || defined(EFIX64) || defined(EFI32)
#include <unistd.h>
#endif

#define HTTP_RESPONSE \
    "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n" \
    "<h2>mbed TLS Test Server</h2>\r\n" \
    "<p>Successful connection using: %s</p>\r\n"

#define DEBUG_LEVEL 0

static void my_debug( void *ctx, int level,
                      const char *file, int line,
                      const char *str )
{
    ((void) level);

    mbedtls_fprintf( (FILE *) ctx, "%s:%04d: %s", file, line, str );
    fflush(  (FILE *) ctx  );
}

int main( void )
{
    int ret, len, cnt = 0, pid;
    int listen_fd;
    int client_fd = -1;
    unsigned char buf[1024];
    const char *pers = "ssl_fork_server";

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt srvcert;
    mbedtls_pk_context pkey;

    mbedtls_ssl_init( &ssl );
    mbedtls_ssl_config_init( &conf );
    mbedtls_entropy_init( &entropy );
    mbedtls_pk_init( &pkey );
    mbedtls_x509_crt_init( &srvcert );
    mbedtls_ctr_drbg_init( &ctr_drbg );

    signal( SIGCHLD, SIG_IGN );

    /*
     * 0. Initial seeding of the RNG
     */
    mbedtls_printf( "\n  . Initial seeding of the random generator..." );
    fflush( stdout );

    if( ( ret = mbedtls_ctr_drbg_seed( &ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *) pers,
                               strlen( pers ) ) ) != 0 )
    {
        mbedtls_printf( " failed\n  ! mbedtls_ctr_drbg_seed returned %d\n", ret );
        goto exit;
    }

    mbedtls_printf( " ok\n" );

    /*
     * 1. Load the certificates and private RSA key
     */
    mbedtls_printf( "  . Loading the server cert. and key..." );
    fflush( stdout );

    /*
     * This demonstration program uses embedded test certificates.
     * Instead, you may want to use mbedtls_x509_crt_parse_file() to read the
     * server and CA certificates, as well as mbedtls_pk_parse_keyfile().
     */
    ret = mbedtls_x509_crt_parse( &srvcert, (const unsigned char *) mbedtls_test_srv_crt,
                          mbedtls_test_srv_crt_len );
    if( ret != 0 )
    {
        mbedtls_printf( " failed\n  !  mbedtls_x509_crt_parse returned %d\n\n", ret );
        goto exit;
    }

    ret = mbedtls_x509_crt_parse( &srvcert, (const unsigned char *) mbedtls_test_cas_pem,
                          mbedtls_test_cas_pem_len );
    if( ret != 0 )
    {
        mbedtls_printf( " failed\n  !  mbedtls_x509_crt_parse returned %d\n\n", ret );
        goto exit;
    }

    ret =  mbedtls_pk_parse_key( &pkey, (const unsigned char *) mbedtls_test_srv_key,
                          mbedtls_test_srv_key_len, NULL, 0 );
    if( ret != 0 )
    {
        mbedtls_printf( " failed\n  !  mbedtls_pk_parse_key returned %d\n\n", ret );
        goto exit;
    }

    mbedtls_printf( " ok\n" );

    /*
     * 1b. Prepare SSL configuration
     */
    mbedtls_printf( "  . Configuring SSL..." );
    fflush( stdout );

    if( ( ret = mbedtls_ssl_config_defaults( &conf,
                    MBEDTLS_SSL_IS_SERVER,
                    MBEDTLS_SSL_TRANSPORT_STREAM,
                    MBEDTLS_SSL_PRESET_DEFAULT ) ) != 0 )
    {
        mbedtls_printf( " failed\n  ! mbedtls_ssl_config_defaults returned %d\n\n", ret );
        goto exit;
    }

    mbedtls_ssl_conf_rng( &conf, mbedtls_ctr_drbg_random, &ctr_drbg );
    mbedtls_ssl_conf_dbg( &conf, my_debug, stdout );

    mbedtls_ssl_conf_ca_chain( &conf, srvcert.next, NULL );
    if( ( ret = mbedtls_ssl_conf_own_cert( &conf, &srvcert, &pkey ) ) != 0 )
    {
        mbedtls_printf( " failed\n  ! mbedtls_ssl_conf_own_cert returned %d\n\n", ret );
        goto exit;
    }

    mbedtls_printf( " ok\n" );

    /*
     * 2. Setup the listening TCP socket
     */
    mbedtls_printf( "  . Bind on https://localhost:4433/ ..." );
    fflush( stdout );

    if( ( ret = mbedtls_net_bind( &listen_fd, NULL, "4433", MBEDTLS_NET_PROTO_TCP ) ) != 0 )
    {
        mbedtls_printf( " failed\n  ! mbedtls_net_bind returned %d\n\n", ret );
        goto exit;
    }

    mbedtls_printf( " ok\n" );

    while( 1 )
    {
        /*
         * 3. Wait until a client connects
         */
        client_fd = -1;
        memset( &ssl, 0, sizeof( ssl ) );

        mbedtls_printf( "  . Waiting for a remote connection ..." );
        fflush( stdout );

        if( ( ret = mbedtls_net_accept( listen_fd, &client_fd,
                                        NULL, 0, NULL ) ) != 0 )
        {
            mbedtls_printf( " failed\n  ! mbedtls_net_accept returned %d\n\n", ret );
            goto exit;
        }

        mbedtls_printf( " ok\n" );

        /*
         * 3.5. Forking server thread
         */

        pid = fork();

        mbedtls_printf( "  . Forking to handle connection ..." );
        fflush( stdout );

        if( pid < 0 )
        {
            mbedtls_printf(" failed\n  ! fork returned %d\n\n", pid );
            goto exit;
        }

        mbedtls_printf( " ok\n" );

        if( pid != 0 )
        {
            if( ( ret = mbedtls_ctr_drbg_reseed( &ctr_drbg,
                                         (const unsigned char *) "parent",
                                         6 ) ) != 0 )
            {
                mbedtls_printf( " failed\n  ! mbedtls_ctr_drbg_reseed returned %d\n", ret );
                goto exit;
            }

            close( client_fd );
            continue;
        }

        close( listen_fd );

        /*
         * 4. Setup stuff
         */
        mbedtls_printf( "  . Setting up the SSL data...." );
        fflush( stdout );

        if( ( ret = mbedtls_ctr_drbg_reseed( &ctr_drbg,
                                     (const unsigned char *) "child",
                                     5 ) ) != 0 )
        {
            mbedtls_printf( " failed\n  ! mbedtls_ctr_drbg_reseed returned %d\n", ret );
            goto exit;
        }

        if( ( ret = mbedtls_ssl_setup( &ssl, &conf ) ) != 0 )
        {
            mbedtls_printf( " failed\n  ! mbedtls_ssl_setup returned %d\n\n", ret );
            goto exit;
        }

        mbedtls_ssl_set_bio( &ssl, &client_fd, mbedtls_net_send, mbedtls_net_recv, NULL );

        mbedtls_printf( " ok\n" );

        /*
         * 5. Handshake
         */
        mbedtls_printf( "  . Performing the SSL/TLS handshake..." );
        fflush( stdout );

        while( ( ret = mbedtls_ssl_handshake( &ssl ) ) != 0 )
        {
            if( ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE )
            {
                mbedtls_printf( " failed\n  ! mbedtls_ssl_handshake returned %d\n\n", ret );
                goto exit;
            }
        }

        mbedtls_printf( " ok\n" );

        /*
         * 6. Read the HTTP Request
         */
        mbedtls_printf( "  < Read from client:" );
        fflush( stdout );

        do
        {
            len = sizeof( buf ) - 1;
            memset( buf, 0, sizeof( buf ) );
            ret = mbedtls_ssl_read( &ssl, buf, len );

            if( ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE )
                continue;

            if( ret <= 0 )
            {
                switch( ret )
                {
                    case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
                        mbedtls_printf( " connection was closed gracefully\n" );
                        break;

                    case MBEDTLS_ERR_NET_CONN_RESET:
                        mbedtls_printf( " connection was reset by peer\n" );
                        break;

                    default:
                        mbedtls_printf( " mbedtls_ssl_read returned %d\n", ret );
                        break;
                }

                break;
            }

            len = ret;
            mbedtls_printf( " %d bytes read\n\n%s", len, (char *) buf );

            if( ret > 0 )
                break;
        }
        while( 1 );

        /*
         * 7. Write the 200 Response
         */
        mbedtls_printf( "  > Write to client:" );
        fflush( stdout );

        len = sprintf( (char *) buf, HTTP_RESPONSE,
                mbedtls_ssl_get_ciphersuite( &ssl ) );

        while( cnt++ < 100 )
        {
            while( ( ret = mbedtls_ssl_write( &ssl, buf, len ) ) <= 0 )
            {
                if( ret == MBEDTLS_ERR_NET_CONN_RESET )
                {
                    mbedtls_printf( " failed\n  ! peer closed the connection\n\n" );
                    goto exit;
                }

                if( ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE )
                {
                    mbedtls_printf( " failed\n  ! mbedtls_ssl_write returned %d\n\n", ret );
                    goto exit;
                }
            }
            len = ret;
            mbedtls_printf( " %d bytes written\n\n%s\n", len, (char *) buf );

            mbedtls_net_usleep( 1000000 );
        }

        mbedtls_ssl_close_notify( &ssl );
        goto exit;
    }

exit:

    if( client_fd != -1 )
        mbedtls_net_close( client_fd );

    mbedtls_x509_crt_free( &srvcert );
    mbedtls_pk_free( &pkey );
    mbedtls_ssl_free( &ssl );
    mbedtls_ssl_config_free( &conf );
    mbedtls_ctr_drbg_free( &ctr_drbg );
    mbedtls_entropy_free( &entropy );

#if defined(_WIN32)
    mbedtls_printf( "  Press Enter to exit this program.\n" );
    fflush( stdout ); getchar();
#endif

    return( ret );
}
#endif /* MBEDTLS_BIGNUM_C && MBEDTLS_CERTS_C && MBEDTLS_ENTROPY_C &&
          MBEDTLS_SSL_TLS_C && MBEDTLS_SSL_SRV_C && MBEDTLS_NET_C &&
          MBEDTLS_RSA_C && MBEDTLS_CTR_DRBG_C && MBEDTLS_PEM_PARSE_C &&
          ! _WIN32 */
