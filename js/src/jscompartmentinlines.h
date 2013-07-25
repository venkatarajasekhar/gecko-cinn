/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jscompartmentinlines_h
#define jscompartmentinlines_h

#include "jscompartment.h"

#include "jscntxtinlines.h"

inline void
JSCompartment::initGlobal(js::GlobalObject &global)
{
    JS_ASSERT(global.compartment() == this);
    JS_ASSERT(!global_);
    global_ = &global;
}

js::GlobalObject *
JSCompartment::maybeGlobal() const
{
    JS_ASSERT_IF(global_, global_->compartment() == this);
    return global_;
}

js::AutoCompartment::AutoCompartment(JSContext *cx, JSObject *target)
  : cx_(cx),
    origin_(cx->compartment())
{
    cx_->enterCompartment(target->compartment());
}

js::AutoCompartment::AutoCompartment(JSContext *cx, JSCompartment *target)
  : cx_(cx),
    origin_(cx->compartment())
{
    cx_->enterCompartment(target);
}

js::AutoCompartment::~AutoCompartment()
{
    cx_->leaveCompartment(origin_);
}

namespace js {

/*
 * Entering the atoms comaprtment is not possible with the AutoCompartment
 * since the atoms compartment does not have a global.
 *
 * Note: since most of the VM assumes that cx->global is non-null, only a
 * restricted set of (atom creating/destroying) operations may be used from
 * inside the atoms compartment.
 */
class AutoEnterAtomsCompartment
{
    JSContext *cx;
    JSCompartment *oldCompartment;
  public:
    AutoEnterAtomsCompartment(JSContext *cx)
      : cx(cx),
        oldCompartment(cx->compartment())
    {
        cx->setCompartment(cx->runtime()->atomsCompartment);
    }

    ~AutoEnterAtomsCompartment()
    {
        cx->setCompartment(oldCompartment);
    }
};

} /* namespace js */

#endif /* jscompartmentinlines_h */
