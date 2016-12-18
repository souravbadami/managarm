
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <iomanip>
#include <iostream>

#include <cofiber.hpp>

#include "common.hpp"
#include "device.hpp"
#include "vfs.hpp"
#include "process.hpp"
#include "exec.hpp"
//FIXME #include "dev_fs.hpp"
//FIXME #include "pts_fs.hpp"
//FIXME #include "sysfile_fs.hpp"
//FIXME #include "extern_fs.hpp"
#include <posix.pb.h>

bool traceRequests = false;

//FIXME: helx::EventHub eventHub = helx::EventHub::create();
//FIXME: helx::Client mbusConnect;
//FIXME: helx::Pipe ldServerPipe;
//FIXME: helx::Pipe mbusPipe;

COFIBER_ROUTINE(cofiber::no_future, observe(SharedProcess self,
		helix::BorrowedDescriptor thread), ([=] {
	using M = helix::AwaitMechanism;

	while(true) {
		helix::Observe<M> observe;
		helix::submitObserve(thread, &observe, helix::Dispatcher::global());
		COFIBER_AWAIT(observe.future());
		HEL_CHECK(observe.error());

		if(observe.observation() == kHelObserveBreakpoint) {
			struct {
				uintptr_t rip;
				uintptr_t rsp;
			} ip_image;
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsIp, &ip_image));

			uintptr_t gprs[15];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, gprs));

			printf("\e[35mBreakpoint fault\n");
			printf("rax: %.16x, rbx: %.16x, rcx: %.16x\n", gprs[0], gprs[1], gprs[2]);
			printf("rdx: %.16x, rdi: %.16x, rsi: %.16x\n", gprs[3], gprs[4], gprs[5]);
			printf(" r8: %.16x,  r9: %.16x, r10: %.16x\n", gprs[6], gprs[7], gprs[8]);
			printf("r11: %.16x, r12: %.16x, r13: %.16x\n", gprs[9], gprs[10], gprs[11]);
			printf("r14: %.16x, r15: %.16x, rbp: %.16x\n", gprs[12], gprs[13], gprs[14]);
			printf("rip: %.16x, rsp: %.16x\n", ip_image.rip, ip_image.rsp);
			printf("\e[39m");

			// TODO: Instead of resuming here we should raise a signal.
			HEL_CHECK(helResume(thread.getHandle()));
		}else{
			throw std::runtime_error("Unexpected observation");
		}
	}
}))

COFIBER_ROUTINE(cofiber::no_future, serve(SharedProcess self,
		helix::UniqueDescriptor p), ([self, lane = std::move(p)] {
	using M = helix::AwaitMechanism;

	observe(self, lane);

	while(true) {
		helix::Accept<M> accept;
		helix::RecvBuffer<M> recv_req;

		char buffer[256];
		helix::submitAsync(lane, {
			helix::action(&accept, kHelItemAncillary),
			helix::action(&recv_req, buffer, 256)
		}, helix::Dispatcher::global());
		COFIBER_AWAIT accept.future();
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::posix::CntRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.request_type() == managarm::posix::CntReqType::OPEN) {
			auto file = COFIBER_AWAIT open(req.path());
			int fd = self.attachFile(file);
			std::cout << "attach " << fd << std::endl;
			(void)fd;

			helix::SendBuffer<M> send_resp;
			helix::PushDescriptor<M> push_passthrough;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_passthrough, getPassthroughLane(file))
			}, helix::Dispatcher::global());
			
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT push_passthrough.future();
			
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_passthrough.error());
		}else if(req.request_type() == managarm::posix::CntReqType::CLOSE) {
			self.closeFile(req.fd());

			helix::SendBuffer<M> send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size()),
			}, helix::Dispatcher::global());
			
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::DUP2) {
			std::cout << "dup2: " << req.fd() << " -> " << req.newfd() << std::endl;
			auto file = self.getFile(req.fd());
			self.attachFile(req.newfd(), file);

			helix::SendBuffer<M> send_resp;
			helix::PushDescriptor<M> push_passthrough;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_passthrough, getPassthroughLane(file))
			}, helix::Dispatcher::global());
			
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT push_passthrough.future();
			
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_passthrough.error());
		}else if(req.request_type() == managarm::posix::CntReqType::FORK) {
			auto child = fork(self);
	
			HelHandle universe;
			HEL_CHECK(helCreateUniverse(&universe));
	
			HelHandle thread;
			HEL_CHECK(helCreateThread(universe, child.getVmSpace().getHandle(), kHelAbiSystemV,
					(void *)req.child_ip(), (char *)req.child_sp(), 0, &thread));
//			serve(child, helix::UniqueDescriptor(thread));

			helix::SendBuffer<M> send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(1);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size()),
			}, helix::Dispatcher::global());
			
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
			std::cout << "posix: fork complete" << std::endl;
		}else{
			std::cout << "posix: Illegal request" << std::endl;
			helix::SendBuffer<M> send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size())
			}, helix::Dispatcher::global());
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
		}
	}
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "Starting posix-subsystem" << std::endl;
	
	execute(SharedProcess::createInit(), "posix-init");

	while(true)
		helix::Dispatcher::global().dispatch();
}

