// The same service with tonic. The service is written by hand, as tonic-build
// would write it, since there is no protoc here.
use std::convert::Infallible;
use std::future::Future;
use std::pin::Pin;
use std::task::{Context, Poll};

use tonic::body::Body;
use tonic::server::{Grpc, NamedService, UnaryService};
use tonic::{Request, Response, Status};

#[derive(Clone, PartialEq, prost::Message)]
struct HelloRequest {
    #[prost(string, tag = "1")]
    name: String,
}

#[derive(Clone, PartialEq, prost::Message)]
struct HelloReply {
    #[prost(string, tag = "1")]
    message: String,
}

type BoxFuture<T, E> = Pin<Box<dyn Future<Output = Result<T, E>> + Send + 'static>>;

struct SayHello;

impl UnaryService<HelloRequest> for SayHello {
    type Response = HelloReply;
    type Future = BoxFuture<Response<HelloReply>, Status>;
    fn call(&mut self, req: Request<HelloRequest>) -> Self::Future {
        Box::pin(async move {
            let name = req.into_inner().name;
            Ok(Response::new(HelloReply { message: format!("Hello {name}") }))
        })
    }
}

#[derive(Clone)]
struct Greeter;

impl NamedService for Greeter {
    const NAME: &'static str = "helloworld.Greeter";
}

impl tower_service::Service<http::Request<Body>> for Greeter {
    type Response = http::Response<Body>;
    type Error = Infallible;
    type Future = BoxFuture<Self::Response, Self::Error>;

    fn poll_ready(&mut self, _: &mut Context<'_>) -> Poll<Result<(), Self::Error>> {
        Poll::Ready(Ok(()))
    }

    fn call(&mut self, req: http::Request<Body>) -> Self::Future {
        match req.uri().path() {
            "/helloworld.Greeter/SayHello" => Box::pin(async move {
                let mut grpc = Grpc::new(tonic_prost::ProstCodec::default());
                Ok(grpc.unary(SayHello, req).await)
            }),
            _ => Box::pin(async move { Ok(Status::unimplemented("").into_http()) }),
        }
    }
}

#[tokio::main]
async fn main() {
    tonic::transport::Server::builder()
        .add_service(Greeter)
        .serve("127.0.0.1:9000".parse().unwrap())
        .await
        .unwrap();
}
