/* ========================= eCAL LICENSE =================================
 *
 * Copyright (C) 2016 - 2019 Continental Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ========================= eCAL LICENSE =================================
*/

#include <ecal/ecal.h>
#include <ecal/msg/protobuf/publisher.h>

#include <iostream>

#include "person.pb.h"

void OnEvent(const char* topic_name_, const struct eCAL::SPubEventCallbackData* data_)
{
  std::cout << "topic name         : " << topic_name_ << std::endl;
  switch (data_->type)
  {
  case pub_event_connected:
    std::cout << "event            : " << "pub_event_connected" << std::endl;
    break;
  case pub_event_disconnected:
    std::cout << "event            : " << "pub_event_disconnected" << std::endl;
    break;
  // not implemented yet
  case pub_event_dropped:
    std::cout << "event            : " << "pub_event_dropped" << std::endl;
    break;
  case pub_event_update_connection:
    std::cout << "event            : " << "pub_event_update_connection" << std::endl;
    std::cout << "  topic_id       : " << data_->tid << std::endl;
    std::cout << "  topic_encoding : " << data_->tinfo.encoding << std::endl;
    std::cout << "  topic_type     : " << data_->tinfo.type << std::endl;
    //std::cout << "  topic_desc : " << data_->tdesc << std::endl;
    break;
  default:
    std::cout << "event        : " << "unknown" << std::endl;
    break;
  }
  std::cout << std::endl;
}

int main(int argc, char **argv)
{
  // initialize eCAL API
  eCAL::Initialize(argc, argv, "person publisher events");

  // set process state
  eCAL::Process::SetState(proc_sev_healthy, proc_sev_level1, "I feel good !");

  // create a publisher (topic name "person")
  eCAL::protobuf::CPublisher<pb::People::Person> pub("person");

  // add event callback function (_1 = topic_name, _2 = event data struct)
  auto evt_callback = std::bind(OnEvent, std::placeholders::_1, std::placeholders::_2);
  pub.AddEventCallback(pub_event_connected,         evt_callback);
  pub.AddEventCallback(pub_event_disconnected,      evt_callback);
  pub.AddEventCallback(pub_event_update_connection, evt_callback);

  // generate a class instance of Person
  pb::People::Person person;

  // start application and wait for events
  std::cout << "Please start 'person_rec_events sample." << std::endl << std::endl;

  // enter main loop
  auto cnt(0);
  while (eCAL::Ok())
  {
    // set person object content
    person.set_id(++cnt);
    person.set_name("Max");
    person.set_stype(pb::People::Person_SType_MALE);
    person.set_email("max@mail.net");
    person.mutable_dog()->set_name("Brandy");
    person.mutable_house()->set_rooms(4);

    // send the person object
    pub.Send(person);

    // sleep 500 ms
    eCAL::Process::SleepMS(500);
  }

  // finalize eCAL API
  eCAL::Finalize();

  return(0);
}
