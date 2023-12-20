//
//  MUXException.hpp
//  usbmuxd2
//
//  Created by tihmstar on 20.07.23.
//

#ifndef MUXException_hpp
#define MUXException_hpp

#include <libgeneral/exception.hpp>

namespace tihmstar {

EASY_BASE_EXCEPTION(MUXException);

EASY_EXCEPTION(MUXException_client_disconnected,MUXException);
EASY_EXCEPTION(MUXException_device_disconnected,MUXException);
EASY_EXCEPTION(MUXException_graceful_kill,MUXException);
};

#endif /* MUXException_hpp */
