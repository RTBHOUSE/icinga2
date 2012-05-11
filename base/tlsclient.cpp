/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012 Icinga Development Team (http://www.icinga.org/)        *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "i2-base.h"

using namespace icinga;

int I2_EXPORT TLSClient::m_SSLIndex;
bool I2_EXPORT TLSClient::m_SSLIndexInitialized = false;

/**
 * TLSClient
 *
 * Constructor for the TLSClient class.
 *
 * @param role The role of the client.
 * @param sslContext The SSL context for the client.
 */
TLSClient::TLSClient(TCPClientRole role, shared_ptr<SSL_CTX> sslContext) : TCPClient(role)
{
	m_SSLContext = sslContext;
	m_BlockRead = false;
	m_BlockWrite = false;
}

/**
 * NullCertificateDeleter
 *
 * Takes a certificate as an argument. Does nothing.
 *
 * @param certificate An X509 certificate.
 */
void TLSClient::NullCertificateDeleter(X509 *certificate)
{
	/* Nothing to do here. */
}

/**
 * GetClientCertificate
 *
 * Retrieves the X509 certficate for this client.
 *
 * @returns The X509 certificate.
 */
shared_ptr<X509> TLSClient::GetClientCertificate(void) const
{
	return shared_ptr<X509>(SSL_get_certificate(m_SSL.get()), &TLSClient::NullCertificateDeleter);
}

/**
 * GetPeerCertificate
 *
 * Retrieves the X509 certficate for the peer.
 *
 * @returns The X509 certificate.
 */
shared_ptr<X509> TLSClient::GetPeerCertificate(void) const
{
	return shared_ptr<X509>(SSL_get_peer_certificate(m_SSL.get()), X509_free);
}

/**
 * Start
 *
 * Registers the TLS socket and starts processing events for it.
 */
void TLSClient::Start(void)
{
	TCPClient::Start();

	m_SSL = shared_ptr<SSL>(SSL_new(m_SSLContext.get()), SSL_free);

	if (!m_SSL)
		throw OpenSSLException("SSL_new failed", ERR_get_error());

	if (!GetClientCertificate())
		throw InvalidArgumentException("No X509 client certificate was specified.");

	if (!m_SSLIndexInitialized) {
		m_SSLIndex = SSL_get_ex_new_index(0, (void *)"TLSClient", NULL, NULL, NULL);
		m_SSLIndexInitialized = true;
	}

	SSL_set_ex_data(m_SSL.get(), m_SSLIndex, this);

	SSL_set_verify(m_SSL.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
	    &TLSClient::SSLVerifyCertificate);

	BIO *bio = BIO_new_socket(GetFD(), 0);
	SSL_set_bio(m_SSL.get(), bio, bio);

	if (GetRole() == RoleInbound)
		SSL_set_accept_state(m_SSL.get());
	else
		SSL_set_connect_state(m_SSL.get());

	SSL_do_handshake(m_SSL.get());
}

/**
 * ReadableEventHandler
 *
 * Processes data that is available for this socket.
 *
 * @param - Event arguments.
 * @returns 0
 */
int TLSClient::ReadableEventHandler(const EventArgs&)
{
	int rc;

	m_BlockRead = false;
	m_BlockWrite = false;

	size_t bufferSize = FIFO::BlockSize / 2;
	char *buffer = (char *)GetRecvQueue()->GetWriteBuffer(&bufferSize);
	rc = SSL_read(m_SSL.get(), buffer, bufferSize);

	if (rc <= 0) {
		switch (SSL_get_error(m_SSL.get(), rc)) {
			case SSL_ERROR_WANT_WRITE:
				m_BlockRead = true;
				/* fall through */
			case SSL_ERROR_WANT_READ:
				return 0;
			case SSL_ERROR_ZERO_RETURN:
				Close();

				return 0;
			default:
				HandleSSLError();

				return 0;
		}
	}

	GetRecvQueue()->Write(NULL, rc);

	EventArgs dea;
	dea.Source = shared_from_this();
	OnDataAvailable(dea);

	return 0;
}

/**
 * WritableEventHandler
 *
 * Processes data that can be written for this socket.
 *
 * @param - Event arguments.
 * @returns 0
 */
int TLSClient::WritableEventHandler(const EventArgs&)
{
	int rc;

	m_BlockRead = false;
	m_BlockWrite = false;

	rc = SSL_write(m_SSL.get(), (const char *)GetSendQueue()->GetReadBuffer(), GetSendQueue()->GetSize());

	if (rc <= 0) {
		switch (SSL_get_error(m_SSL.get(), rc)) {
			case SSL_ERROR_WANT_READ:
				m_BlockWrite = true;
				/* fall through */
			case SSL_ERROR_WANT_WRITE:
				return 0;
			case SSL_ERROR_ZERO_RETURN:
				Close();

				return 0;
			default:
				HandleSSLError();

				return 0;
		}
	}

	GetSendQueue()->Read(NULL, rc);

	return 0;
}

/**
 * WantsToRead
 *
 * Checks whether data should be read for this socket.
 *
 * @returns true if data should be read, false otherwise.
 */
bool TLSClient::WantsToRead(void) const
{
	if (SSL_want_read(m_SSL.get()))
		return true;

	if (m_BlockRead)
		return false;

	return TCPClient::WantsToRead();
}

/**
 * WantsToWrite
 *
 * Checks whether data should be written for this socket.
 *
 * @returns true if data should be written, false otherwise.
 */
bool TLSClient::WantsToWrite(void) const
{
	if (SSL_want_write(m_SSL.get()))
		return true;

	if (m_BlockWrite)
		return false;

	return TCPClient::WantsToWrite();
}

/**
 * CloseInternal
 *
 * Closes the socket.
 *
 * @param from_dtor Whether this method was invoked from the destructor.
 */
void TLSClient::CloseInternal(bool from_dtor)
{
	SSL_shutdown(m_SSL.get());

	TCPClient::CloseInternal(from_dtor);
}

/**
 * HandleSSLError
 *
 * Handles an OpenSSL error.
 */
void TLSClient::HandleSSLError(void)
{
	int code = ERR_get_error();

	if (code != 0) {
		SocketErrorEventArgs sea;
		sea.Code = code;
		sea.Message = OpenSSLException::FormatErrorCode(sea.Code);
		OnError(sea);
	}

	Close();
	return;
}

/**
 * TLSClientFactory
 *
 * Factory function for the TLSClient class.
 *
 * @param role The role of the TLS socket.
 * @param sslContext The SSL context for the socket.
 * @returns A new TLS socket.
 */
TCPClient::Ptr icinga::TLSClientFactory(TCPClientRole role, shared_ptr<SSL_CTX> sslContext)
{
	return make_shared<TLSClient>(role, sslContext);
}

/**
 * SSLVerifyCertificate
 *
 * Callback function that verifies SSL certificates.
 *
 * @param ok Whether pre-checks for the SSL certificates were successful.
 * @param x509Context X509 context for the certificate.
 * @returns 1 if the verification was successful, 0 otherwise.
 */
int TLSClient::SSLVerifyCertificate(int ok, X509_STORE_CTX *x509Context)
{
	SSL *ssl = (SSL *)X509_STORE_CTX_get_ex_data(x509Context, SSL_get_ex_data_X509_STORE_CTX_idx());
	TLSClient *client = (TLSClient *)SSL_get_ex_data(ssl, m_SSLIndex);

	if (client == NULL)
		return 0;

	VerifyCertificateEventArgs vcea;
	vcea.Source = client->shared_from_this();
	vcea.ValidCertificate = (ok != 0);
	vcea.Context = x509Context;
	vcea.Certificate = shared_ptr<X509>(x509Context->cert, &TLSClient::NullCertificateDeleter);
	client->OnVerifyCertificate(vcea);

	return (int)vcea.ValidCertificate;
}
