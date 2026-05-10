#include "EventBus.h"

void EventBus::subscribe(EventType event, EventCallback cb) {
    subscribers.push_back({ event, cb });
}

void EventBus::publish(EventType event, const void* payload) {
    for (auto& subscriber : subscribers) {
        if (subscriber.event == event) {
            subscriber.cb(event, payload);
        }
    }
}