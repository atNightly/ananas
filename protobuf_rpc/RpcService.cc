#include <google/protobuf/service.h>
#include <google/protobuf/descriptor.h>

#include "RpcService.h"
#include "RpcClosure.h"
#include "RpcException.h"
#include "ProtobufCoder.h"
#include "ananas_rpc.pb.h"
#include "net/Connection.h"
#include "net/Application.h"
#include "net/AnanasDebug.h"
#include "util/Util.h"

namespace ananas
{

namespace rpc
{

Service::Service(google::protobuf::Service* service) :
    service_(service),
    name_(service->GetDescriptor()->full_name())
{
}

Service::~Service()
{
}

google::protobuf::Service* Service::GetService() const
{
    return service_.get();
}

void Service::SetBindAddr(const SocketAddr& addr)
{
    assert (!bindAddr_.IsValid());
    bindAddr_ = addr;
}

bool Service::Start()
{
    if (!bindAddr_.IsValid())
        return false;

    auto& app = ananas::Application::Instance();
    app.Listen(bindAddr_, std::bind(&Service::OnNewConnection,
                                    this,
                                    std::placeholders::_1));
    return true;
}

const std::string& Service::FullName() const
{
    return name_;
}

void Service::OnNewConnection(ananas::Connection* conn)
{
    auto channel = std::make_shared<ServerChannel>(conn, this);
    conn->SetUserData(channel);

    assert (conn->GetLoop()->Id() < static_cast<int>(channels_.size()));

    auto& channelMap = channels_[conn->GetLoop()->Id()];
    bool succ = channelMap.insert({conn->GetUniqueId(), channel.get()}).second;
    assert (succ);

    if (onCreateChannel_)
        onCreateChannel_(channel.get());

    conn->SetOnDisconnect(std::bind(&Service::_OnDisconnect, this, std::placeholders::_1));
    conn->SetOnMessage(&Service::_OnMessage);
}

void Service::OnRegister()
{
    channels_.resize(Application::Instance().NumOfWorker());
}

void Service::SetMethodSelector(std::function<const char* (const google::protobuf::Message* )> ms)
{
    methodSelector_ = std::move(ms);
}

void Service::SetOnCreateChannel(std::function<void (ServerChannel* )> occ)
{
    onCreateChannel_ = std::move(occ);
}

size_t Service::_OnMessage(ananas::Connection* conn, const char* data, size_t len)
{
    const char* const start = data;
    size_t offset = 0;

    auto channel = conn->GetUserData<ServerChannel>();

    // TODO process message like redis
    try {
        // 如果是二进制消息，这里进行包的完整性分析，如果是完整的包，将得到了一个RpcMessage,还需要进一步解包
        // 如果是文本消息，包的完整性分析和解包是一步完成的，直接得到Message，无须再解包
        const char* const thisStart = data;
        auto msg = channel->OnData(data, len - offset);
        if (msg)
        {
            offset += (data - thisStart);
            try {
                channel->OnMessage(std::move(msg));
            }
            catch (const std::logic_error& recovableErr) {
                channel->OnError(recovableErr);
                return data - start;
            }
            catch (const std::runtime_error& unrecovableErr) {
                channel->OnError(unrecovableErr);
                conn->ActiveClose();
                return data - start;
            }
            catch (...) {
                ANANAS_ERR << "OnMessage: Unknown error";
                conn->ActiveClose();
                return data - start;
            }
        }
    }
    catch (const std::exception& e) {
        // Often because evil message
        ANANAS_ERR << "Some exception OnData " << e.what();
        conn->ActiveClose();
        return 0;
    }

    return data - start;
}

void Service::_OnDisconnect(ananas::Connection* conn)
{
    auto& channelMap = channels_[conn->GetLoop()->Id()];
    bool succ = channelMap.erase(conn->GetUniqueId());
    assert (succ);
}

ServerChannel::ServerChannel(ananas::Connection* conn, ananas::rpc::Service* service) :
    conn_(conn),
    service_(service),
    encoder_(PbToFrameResponseEncoder)
{
}

ServerChannel::~ServerChannel()
{
}

ananas::rpc::Service* ServerChannel::Service() const
{
    return service_;
}

ananas::Connection* ServerChannel::Connection() const
{
    return conn_;
}

void ServerChannel::SetEncoder(Encoder enc)
{
    encoder_ = std::move(enc);
}

void ServerChannel::SetDecoder(Decoder dec)
{
    decoder_ = std::move(dec);
}

std::shared_ptr<google::protobuf::Message> ServerChannel::OnData(const char*& data, size_t len)
{
    return decoder_.b2mDecoder_(data, len);
}

bool ServerChannel::OnMessage(std::shared_ptr<google::protobuf::Message> req)
{
    std::string method;
    //int id = 0;
    RpcMessage* frame = dynamic_cast<RpcMessage*>(req.get());
    if (frame)
    {
        if (frame->has_request())
        {
            currentId_ = frame->request().id();
            method = frame->request().method_name();

            if (frame->request().service_name() != 
                service_->FullName())
                throw NoServiceException("Not find service [" + frame->request().service_name() + "]");
        }
        else
            throw NoRequestException();
    }
    else
    {
        currentId_ = -1;
        if (!service_->methodSelector_)
        {
            // error, need methodSelector_
            ANANAS_ERR << "How to get method name from message? You forget to set methodSelector";
            throw MethodUndeterminedException("methodSelector not set for [" + service_->FullName() + "]");
        }
        else
        {
            method = service_->methodSelector_(req.get());
            ANANAS_DBG << "Debug: get method [" << method.data() << "] from message";
        }
    }

    this->_Invoke(method, std::move(req));
    return true;
}
    
void ServerChannel::_Invoke(const std::string& methodName, std::shared_ptr<google::protobuf::Message> req)
{
    const auto googServ = service_->GetService();
    auto method = googServ->GetDescriptor()->FindMethodByName(methodName);
    if (!method)
    {
        ANANAS_ERR << "_Invoke: No such method " << methodName;
        throw MethodUndeterminedException("Not find method [" + methodName + "]");
    }

    if (decoder_.m2mDecoder_)
    {
        std::unique_ptr<google::protobuf::Message> request(googServ->GetRequestPrototype(method).New()); 
        decoder_.m2mDecoder_(*req, *request); // may be ParseFromString
        req.reset(request.release());
    }

    /*
     * The resource manage is a little dirty, because CallMethod accepts raw pointers.
     * Here is my solution:
     * 1. request must be delete when exit this function, so if you want to process request
     * async, you MUST copy it.
     * 2. response is managed by Closure, so use shared_ptr.
     * 3. Closure must be managed by raw pointer, so when call Closure::Run, it will execute
     * `delete this` when exit, at which time the response is also destroyed.
     */
    std::shared_ptr<google::protobuf::Message> response(googServ->GetResponsePrototype(method).New()); 

    std::weak_ptr<ananas::Connection> wconn(std::static_pointer_cast<ananas::Connection>(conn_->shared_from_this()));
    googServ->CallMethod(method, nullptr, req.get(), response.get(), 
            new ananas::rpc::Closure(&ServerChannel::_OnServDone, this, wconn, currentId_, response));
}

void ServerChannel::_OnServDone(std::weak_ptr<ananas::Connection> wconn,
                                int id,
                                std::shared_ptr<google::protobuf::Message> response)
{
    auto conn = wconn.lock();
    if (!conn) return;

    RpcMessage frame;
    Response* rsp = frame.mutable_response();
    rsp->set_id(id);
    bool succ = encoder_.m2fEncoder_(response.get(), frame);
    assert (succ);

    if (encoder_.f2bEncoder_)
    {
        ananas::Buffer bytes = encoder_.f2bEncoder_(frame);
        conn->SendPacket(bytes.ReadAddr(), bytes.ReadableSize());
    }
    else
    {
        const auto& bytes = rsp->serialized_response();
        conn->SendPacket(bytes.data(), bytes.size());
    }
}


void ServerChannel::OnError(const std::exception& err)
{
    RpcMessage frame;
    Response& rsp = *frame.mutable_response();
    if (currentId_ != -1) rsp.set_id(currentId_);
    rsp.mutable_error()->set_msg(err.what());
    bool succ = encoder_.m2fEncoder_(nullptr, frame);
    assert (succ);

    if (encoder_.f2bEncoder_)
    {
        ananas::Buffer bytes = encoder_.f2bEncoder_(frame);
        conn_->SendPacket(bytes.ReadAddr(), bytes.ReadableSize());
    }
    else
    {
        const auto& bytes = rsp.serialized_response();
        conn_->SendPacket(bytes.data(), bytes.size());
    }
}


} // end namespace rpc

} // end namespace ananas
