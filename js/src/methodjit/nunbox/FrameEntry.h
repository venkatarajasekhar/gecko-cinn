/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla SpiderMonkey JavaScript 1.9 code, released
 * May 28, 2008.
 *
 * The Initial Developer of the Original Code is
 *   Brendan Eich <brendan@mozilla.org>
 *
 * Contributor(s):
 *   David Anderson <danderson@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#if !defined jsjaeger_valueinfo_h__ && defined JS_METHODJIT
#define jsjaeger_valueinfo_h__

#include "jsapi.h"
#include "methodjit/MachineRegs.h"
#include "methodjit/RematInfo.h"
#include "assembler/assembler/MacroAssembler.h"

namespace js {
namespace mjit {

class FrameEntry
{
    friend class FrameState;

  public:
    bool isConstant() {
        return data.isConstant();
    }

    const jsval_layout &getConstant() {
        JS_ASSERT(isConstant());
        return v_;
    }

    const Value &getValue() {
        JS_ASSERT(isConstant());
        return Valueify(v_.asBits);
    }

    bool isTypeConstant() {
        return type.isConstant();
    }

    uint32 getTypeTag() {
        return v_.s.mask32;
    }

    uint32 getPayload32() {
        JS_ASSERT(!Valueify(v_.asBits).isDouble() || type.synced());
        return v_.s.payload.u32;
    }

    uint32 copyOf() {
        JS_ASSERT(type.isCopy() || data.isCopy());
        return index_;
    }

  private:
    void setConstant(const jsval &v) {
        type.setConstant();
        type.unsync();
        data.setConstant();
        data.unsync();
        v_.asBits = v;
    }

  private:
    RematInfo  type;
    RematInfo  data;
    jsval_layout v_;
    uint32     index_;
    uint32     copies;
};

} /* namespace mjit */
} /* namespace js */

#endif /* jsjaeger_valueinfo_h__ */

