// The same service with grpc-go. With no protoc here, the messages are
// read and written with protowire through a codec of raw bytes.
package main

import (
	"context"
	"net"

	"google.golang.org/grpc"
	"google.golang.org/protobuf/encoding/protowire"
)

type raw struct{}

func (raw) Marshal(v any) ([]byte, error)   { return *(v.(*[]byte)), nil }
func (raw) Unmarshal(b []byte, v any) error { *(v.(*[]byte)) = append([]byte(nil), b...); return nil }
func (raw) Name() string                    { return "proto" }

func name(b []byte) string {
	for len(b) > 0 {
		num, typ, n := protowire.ConsumeTag(b)
		if n < 0 {
			return ""
		}
		b = b[n:]
		if num == 1 && typ == protowire.BytesType {
			v, _ := protowire.ConsumeString(b)
			return v
		}
		b = b[protowire.ConsumeFieldValue(num, typ, b):]
	}
	return ""
}

func sayHello(srv any, ctx context.Context, dec func(any) error, _ grpc.UnaryServerInterceptor) (any, error) {
	var in []byte
	if err := dec(&in); err != nil {
		return nil, err
	}
	out := protowire.AppendTag(nil, 1, protowire.BytesType)
	out = protowire.AppendString(out, "Hello "+name(in))
	return &out, nil
}

func main() {
	s := grpc.NewServer(grpc.ForceServerCodec(raw{}))
	s.RegisterService(&grpc.ServiceDesc{
		ServiceName: "helloworld.Greeter",
		HandlerType: (*any)(nil),
		Methods:     []grpc.MethodDesc{{MethodName: "SayHello", Handler: sayHello}},
	}, struct{}{})
	ln, err := net.Listen("tcp", "127.0.0.1:9000")
	if err != nil {
		panic(err)
	}
	s.Serve(ln)
}
