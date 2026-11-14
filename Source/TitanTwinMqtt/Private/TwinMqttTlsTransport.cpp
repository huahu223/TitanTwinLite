// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinMqttTlsTransport.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "SslModule.h"
#include "Interfaces/ISslCertificateManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
THIRD_PARTY_INCLUDES_START
#define UI OPENSSL_UI
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#undef UI
THIRD_PARTY_INCLUDES_END

namespace
{
    int BioCreate(BIO* Bio) { BIO_set_init(Bio,1); BIO_set_data(Bio,nullptr); return 1; }
    int BioDestroy(BIO* Bio) { if(Bio)BIO_set_data(Bio,nullptr); return 1; }
    long BioControl(BIO*,int Cmd,long,void*) { return Cmd==BIO_CTRL_FLUSH ? 1 : 0; }
    bool WouldBlock()
    {
        auto* S=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        return S && (S->GetLastErrorCode()==SE_EWOULDBLOCK || S->GetLastErrorCode()==SE_EINPROGRESS);
    }
    int BioRead(BIO* Bio,char* Out,int Size)
    {
        BIO_clear_retry_flags(Bio); auto* Socket=static_cast<FSocket*>(BIO_get_data(Bio)); int32 Count=0;
        if(!Socket||!Out||Size<=0)return 0;
        // UE's stream Recv returns true + zero bytes for EWOULDBLOCK, false + zero for EOF.
        if(Socket->Recv(reinterpret_cast<uint8*>(Out),Size,Count))
        {
            if(Count>0)return Count;
            BIO_set_retry_read(Bio);return -1;
        }
        return Count==0 ? 0 : -1;
    }
    int BioWrite(BIO* Bio,const char* In,int Size)
    {
        BIO_clear_retry_flags(Bio); auto* Socket=static_cast<FSocket*>(BIO_get_data(Bio)); int32 Count=0;
        if(!Socket||!In||Size<=0)return 0;
        if(Socket->Send(reinterpret_cast<const uint8*>(In),Size,Count)&&Count>0)return Count;
        if(WouldBlock())BIO_set_retry_write(Bio);
        return -1;
    }
    FString SslError(const TCHAR* Action)
    {
        const unsigned long Code=ERR_get_error(); char Text[256]{};
        if(Code)ERR_error_string_n(Code,Text,sizeof(Text));
        return FString::Printf(TEXT("%s: %s"),Action,Code?UTF8_TO_TCHAR(Text):TEXT("TLS connection failed"));
    }
}

FTwinMqttTlsTransport::~FTwinMqttTlsTransport()
{
    if(Session)SSL_free(Session);
    if(Context)SSL_CTX_free(Context);
    if(Method)BIO_meth_free(Method);
}
bool FTwinMqttTlsTransport::Connect(FSocket* Socket,const FString& Host,const FString& CaFile,
    double Deadline,const std::atomic<bool>& Stop,FString& Error)
{
    ServerHost=Host; ERR_clear_error();
    Context=SSL_CTX_new(TLS_client_method());
    if(!Context){Error=SslError(TEXT("Create TLS context"));return false;}
    if(SSL_CTX_set_min_proto_version(Context,TLS1_2_VERSION)!=1){Error=SslError(TEXT("Set TLS minimum"));return false;}
    SSL_CTX_set_options(Context,SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify(Context,SSL_VERIFY_PEER,[](int Ok,X509_STORE_CTX* Store)->int
    {
        if(!Ok)return 0;
        if(X509_STORE_CTX_get_error_depth(Store)!=0)return 1;
        auto* S=static_cast<SSL*>(X509_STORE_CTX_get_ex_data(Store,SSL_get_ex_data_X509_STORE_CTX_idx()));
        auto* Self=S?static_cast<FTwinMqttTlsTransport*>(SSL_get_app_data(S)):nullptr;
        return Self && FSslModule::Get().GetCertificateManager().VerifySslCertificates(Store,Self->ServerHost) ? 1 : 0;
    });
    FSslModule::Get().GetCertificateManager().AddCertificatesToSslContext(Context);
    if(!CaFile.IsEmpty())
    {
        const FString Path=FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(),CaFile);
        const int64 Size=IFileManager::Get().FileSize(*Path); TArray<uint8> Bytes;
        if(Size<=0||Size>2*1024*1024||!FFileHelper::LoadFileToArray(Bytes,*Path))
        {Error=TEXT("TLS CA file missing, empty or over 2 MiB");return false;}
        BIO* Input=BIO_new_mem_buf(Bytes.GetData(),Bytes.Num()); int Parsed=0;
        if(!Input){Error=SslError(TEXT("Read CA file"));return false;}
        while(X509* Cert=PEM_read_bio_X509(Input,nullptr,nullptr,nullptr))
        {
            if(X509_STORE_add_cert(SSL_CTX_get_cert_store(Context),Cert)!=1)
            {
                const unsigned long Code=ERR_peek_last_error();
                if(ERR_GET_REASON(Code)!=X509_R_CERT_ALREADY_IN_HASH_TABLE)
                {X509_free(Cert);BIO_free(Input);Error=SslError(TEXT("Trust CA certificate"));return false;}
                ERR_clear_error();
            }
            ++Parsed; X509_free(Cert);
        }
        BIO_free(Input); ERR_clear_error();
        if(!Parsed){Error=TEXT("TLS CA file contains no PEM certificates");return false;}
    }
    Method=BIO_meth_new(BIO_TYPE_SOURCE_SINK|BIO_get_new_index(),"TitanTwin socket");
    if(!Method){Error=SslError(TEXT("Create TLS IO"));return false;}
    BIO_meth_set_create(Method,BioCreate); BIO_meth_set_destroy(Method,BioDestroy);
    BIO_meth_set_read(Method,BioRead); BIO_meth_set_write(Method,BioWrite); BIO_meth_set_ctrl(Method,BioControl);
    Session=SSL_new(Context); BIO* Io=BIO_new(Method);
    if(!Session||!Io){if(Io)BIO_free(Io);Error=SslError(TEXT("Create TLS session"));return false;}
    BIO_set_data(Io,Socket); SSL_set_bio(Session,Io,Io); SSL_set_app_data(Session,this);
    SSL_set_mode(Session,SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    FTCHARToUTF8 Name(*Host);
    X509_VERIFY_PARAM* Param=SSL_get0_param(Session);
    X509_VERIFY_PARAM_set_hostflags(Param,X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    // Numeric endpoints must match an IP SAN, never a DNS common-name string.
    if(X509_VERIFY_PARAM_set1_ip_asc(Param,Name.Get())!=1)
    {
        if(SSL_set1_host(Session,Name.Get())!=1||SSL_set_tlsext_host_name(Session,Name.Get())!=1)
        {Error=SslError(TEXT("Configure TLS hostname"));return false;}
    }
    while(!Stop.load(std::memory_order_acquire)&&FPlatformTime::Seconds()<Deadline)
    {
        ERR_clear_error(); const int Count=SSL_connect(Session);
        if(Count==1)
        {
            if(SSL_get_verify_result(Session)!=X509_V_OK){Error=TEXT("TLS peer certificate verification failed");return false;}
            return true;
        }
        bool ReadWait=true;
        if(Result(Count,ReadWait,Error)<0)return false;
        Socket->Wait(ReadWait?ESocketWaitConditions::WaitForRead:ESocketWaitConditions::WaitForWrite,FTimespan::FromMilliseconds(25));
    }
    Error=Stop.load()?TEXT("TLS handshake cancelled"):TEXT("TLS handshake timed out");return false;
}
int32 FTwinMqttTlsTransport::Result(int32 Count,bool& ReadWait,FString& Error)
{
    if(Count>0)return Count;
    const int Code=SSL_get_error(Session,Count);
    if(Code==SSL_ERROR_WANT_READ||Code==SSL_ERROR_WANT_WRITE){ReadWait=Code==SSL_ERROR_WANT_READ;return 0;}
    if(Code==SSL_ERROR_ZERO_RETURN)Error=TEXT("TLS peer closed the connection");
    else Error=SslError(TEXT("MQTT TLS"));
    return -1;
}
int32 FTwinMqttTlsTransport::Read(uint8* Data,int32 Size,bool& ReadWait,FString& Error)
{ ERR_clear_error(); return Result(SSL_read(Session,Data,Size),ReadWait,Error); }
int32 FTwinMqttTlsTransport::Write(const uint8* Data,int32 Size,bool& ReadWait,FString& Error)
{ ERR_clear_error(); return Result(SSL_write(Session,Data,Size),ReadWait,Error); }
bool FTwinMqttTlsTransport::HasPending()const {return Session&&SSL_pending(Session)>0;}
