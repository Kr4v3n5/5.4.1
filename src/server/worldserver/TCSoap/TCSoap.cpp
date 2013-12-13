/*
 * Copyright (C) 2008-2013 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2013 Project Cerberus <http://www.erabattle.ru/>
 *
 * This program is not free software; you can not redistribute it and/or modify it.
 *
 * This program is distributed only by <http://www.erabattle.ru/>!
 */

#include "TCSoap.h"
#include "soapH.h"
#include "soapStub.h"
#include "World.h"
#include "AccountMgr.h"
#include "Log.h"

void TCSoapRunnable::run()
{
    struct soap soap;
    soap_init(&soap);
    soap_set_imode(&soap, SOAP_C_UTFSTRING);
    soap_set_omode(&soap, SOAP_C_UTFSTRING);

    // check every 3 seconds if world ended
    soap.accept_timeout = 3;
    soap.recv_timeout = 5;
    soap.send_timeout = 5;
    if (!soap_valid_socket(soap_bind(&soap, _host.c_str(), _port, 100)))
    {
        TC_LOG_ERROR(LOG_FILTER_SOAP, "Couldn't bind to %s:%d", _host.c_str(), _port);
        exit(-1);
    }

    TC_LOG_INFO(LOG_FILTER_SOAP, "Bound to http://%s:%d", _host.c_str(), _port);

    while (!World::IsStopped())
    {
        if (!soap_valid_socket(soap_accept(&soap)))
            continue;   // ran into an accept timeout

        TC_LOG_DEBUG(LOG_FILTER_SOAP, "Accepted connection from IP=%d.%d.%d.%d", (int)(soap.ip>>24)&0xFF, (int)(soap.ip>>16)&0xFF, (int)(soap.ip>>8)&0xFF, (int)soap.ip&0xFF);
        struct soap* thread_soap = soap_copy(&soap);// make a safe copy

        ACE_Message_Block* mb = new ACE_Message_Block(sizeof(struct soap*));
        ACE_OS::memcpy(mb->wr_ptr(), &thread_soap, sizeof(struct soap*));
        process_message(mb);
    }

    soap_done(&soap);
}

void TCSoapRunnable::process_message(ACE_Message_Block* mb)
{
    ACE_TRACE (ACE_TEXT ("SOAPWorkingThread::process_message"));

    struct soap* soap;
    ACE_OS::memcpy(&soap, mb->rd_ptr (), sizeof(struct soap*));
    mb->release();

    soap_serve(soap);
    soap_destroy(soap); // dealloc C++ data
    soap_end(soap); // dealloc data and clean up
    soap_done(soap); // detach soap struct
    free(soap);
}
/*
Code used for generating stubs:

int ns1__executeCommand(char* command, char** result);
*/
int ns1__executeCommand(soap* soap, char* command, char** result)
{
    // security check
    if (!soap->userid || !soap->passwd)
    {
        TC_LOG_INFO(LOG_FILTER_SOAP, "Client didn't provide login information");
        return 401;
    }

    uint32 accountId = AccountMgr::GetId(soap->userid);
    if (!accountId)
    {
        TC_LOG_INFO(LOG_FILTER_SOAP, "Client used invalid username '%s'", soap->userid);
        return 401;
    }

    if (!AccountMgr::CheckPassword(accountId, soap->passwd))
    {
        TC_LOG_INFO(LOG_FILTER_SOAP, "Invalid password for account '%s'", soap->userid);
        return 401;
    }

    if (AccountMgr::GetSecurity(accountId) < SEC_ADMINISTRATOR)
    {
        TC_LOG_INFO(LOG_FILTER_SOAP, "%s's gmlevel is too low", soap->userid);
        return 403;
    }

    if (!command || !*command)
        return soap_sender_fault(soap, "Command can not be empty", "The supplied command was an empty string");

    TC_LOG_INFO(LOG_FILTER_SOAP, "Received command '%s'", command);
    SOAPCommand connection;

    // commands are executed in the world thread. We have to wait for them to be completed
    {
        // CliCommandHolder will be deleted from world, accessing after queueing is NOT save
        CliCommandHolder* cmd = new CliCommandHolder(&connection, command, &SOAPCommand::print, &SOAPCommand::commandFinished);
        sWorld->QueueCliCommand(cmd);
    }

    // wait for callback to complete command

    int acc = connection.pendingCommands.acquire();
    if (acc)
        TC_LOG_ERROR(LOG_FILTER_SOAP, "Error while acquiring lock, acc = %i, errno = %u", acc, errno);

    // alright, command finished

    char* printBuffer = soap_strdup(soap, connection.m_printBuffer.c_str());
    if (connection.hasCommandSucceeded())
    {
        *result = printBuffer;
        return SOAP_OK;
    }
    else
        return soap_sender_fault(soap, printBuffer, printBuffer);
}

void SOAPCommand::commandFinished(void* soapconnection, bool success)
{
    SOAPCommand* con = (SOAPCommand*)soapconnection;
    con->setCommandSuccess(success);
    con->pendingCommands.release();
}

////////////////////////////////////////////////////////////////////////////////
//
//  Namespace Definition Table
//
////////////////////////////////////////////////////////////////////////////////

struct Namespace namespaces[] =
{   { "SOAP-ENV", "http://schemas.xmlsoap.org/soap/envelope/", NULL, NULL }, // must be first
    { "SOAP-ENC", "http://schemas.xmlsoap.org/soap/encoding/", NULL, NULL }, // must be second
    { "xsi", "http://www.w3.org/1999/XMLSchema-instance", "http://www.w3.org/*/XMLSchema-instance", NULL },
    { "xsd", "http://www.w3.org/1999/XMLSchema",          "http://www.w3.org/*/XMLSchema", NULL },
    { "ns1", "urn:TC", NULL, NULL },     // "ns1" namespace prefix
    { NULL, NULL, NULL, NULL }
};