/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#ifndef DATAENCODE_H
#define DATAENCODE_H

#include <cassert>

#include <stdexcept>
#include <functional>
#include <ostream>
#include <list>
#include <map>
#include <utility>
#include <type_traits>
#include <memory>

#include <pvxs/data.h>
#include <pvxs/sharedArray.h>
#include "pvaproto.h"
#include "utilpvt.h"
#include "dataimpl.h"

namespace pvxs {
namespace impl {

void to_wire(Buffer& buf, const FieldDesc* cur)
{
    // we assume FieldDesc* is valid (checked on creation)
    to_wire(buf, cur->code.code);

    // other than (array of) struct and union, encoding is simple
    switch(cur->code.code) {
    case TypeCode::StructA:
    case TypeCode::UnionA:
        to_wire(buf, cur+1);
        break;

    case TypeCode::Struct:
    case TypeCode::Union:
        to_wire(buf, cur->id);
        to_wire(buf, Size{cur->miter.size()});
        for(auto& pair : cur->miter) {
            to_wire(buf, pair.first);
            to_wire(buf, cur+pair.second); // jump forward in FieldDesc array and recurse!
        }
        break;
    default:
        break;
    }
}

void from_wire(Buffer& buf, TypeDeserContext& ctxt, unsigned depth)
{
    if(!buf.good() || depth>20) {
        buf.fault();
        return;
    }

    TypeCode code;
    from_wire(buf, code.code);
    const size_t index = ctxt.descs.size(); // index of first node we add to ctxt.descs[]

    if(code == TypeCode::Null) {
        return;

    } else if(code.code==0xfd) {
        // update cache
        uint16_t key=0;
        from_wire(buf, key);
        from_wire(buf, ctxt, depth+1u);
        if(!buf.good() || index==ctxt.descs.size()) {
            buf.fault();
            return;

        } else {
            auto& entry = ctxt.cache[key];
            // copy new node, and any decendents into cache
            entry.resize(ctxt.descs.size()-index);
            std::copy(ctxt.descs.begin()+index,
                      ctxt.descs.end(),
                      entry.begin());
        }

    } else if(code.code==0xfe) {
        // fetch cache
        uint16_t key=0;
        from_wire(buf, key);
        auto it = ctxt.cache.find(key);
        if(it==ctxt.cache.end()) {
            buf.fault();
        }

        if(!buf.good() || it->second.empty()) {
            buf.fault();
            return;

        } else {
            // copy from cache
            ctxt.descs.resize(index+it->second.size());
            std::copy(it->second.begin(),
                      it->second.end(),
                      ctxt.descs.begin()+index);
        }

    } else if(code.code!=0xff && code.code&0x10) {
        // fixed length is deprecated
        buf.fault();

    } else {
        // actual field

        ctxt.descs.emplace_back();
        {
            auto& fld = ctxt.descs.back();

            fld.code = code;
            fld.hash = code.code;
        }

        switch(code.code) {
        case TypeCode::StructA:
        case TypeCode::UnionA:
            from_wire(buf, ctxt, depth+1);
            if(!buf.good() || ctxt.descs.size()==index || ctxt.descs[index+1].code!=code.scalarOf()) {
                buf.fault();
                return;
            }
            break;

        case TypeCode::Struct:
        case TypeCode::Union: {
            from_wire(buf, ctxt.descs.back().id);

            Size nfld{0};
            std::string name;
            from_wire(buf, nfld); // number of children
            {
                auto& fld = ctxt.descs.back();

                fld.miter.reserve(nfld.size);
                fld.hash ^= std::hash<std::string>{}(fld.id);
            }

            for(auto i: range(nfld.size)) {
                (void)i;
                const size_t cindex = ctxt.descs.size(); // index of this child
                from_wire(buf, name);
                from_wire(buf, ctxt, depth+1);
                if(!buf.good() || cindex>=ctxt.descs.size()) {
                    buf.fault();
                    return;
                }

                // descs may be re-allocated (invalidating previous refs.)
                auto& fld = ctxt.descs[index];
                auto& cfld = ctxt.descs[cindex];

                // update hash
                // TODO investigate better ways to combine hashes
                fld.hash ^= std::hash<std::string>{}(name) ^ cfld.hash;

                // update field refs.
                fld.miter.emplace_back(name, cindex-index);
                fld.mlookup[name] = cindex-index;
                name+='.';

                if(code.code==TypeCode::Struct && code==cfld.code) {
                    // copy decendent indicies for sub-struct
                    for(auto& pair : cfld.mlookup) {
                        fld.mlookup[name+pair.first] = cindex + pair.second;
                    }
                }
            }
        }
            break;
        default:
            // not handling fixed/bounded
            // other types have simple/single node description
            switch(code.scalarOf().code) {
            case TypeCode::Bool:
            case TypeCode::Int8:
            case TypeCode::Int16:
            case TypeCode::Int32:
            case TypeCode::Int64:
            case TypeCode::UInt8:
            case TypeCode::UInt16:
            case TypeCode::UInt32:
            case TypeCode::UInt64:
            case TypeCode::Float32:
            case TypeCode::Float64:
            case TypeCode::String:
            case TypeCode::Any:
                break;
            default:
                buf.fault();
                break;
            }
        }

        ctxt.descs[index].num_index = ctxt.descs.size()-index;
    }
}

namespace {
template<typename E, typename C = E>
void to_wire(Buffer& buf, const shared_array<const void>& varr)
{
    auto arr = varr.castTo<const E>();
    to_wire(buf, Size{arr.size()});
    for(auto i : range(arr.size())) {
        to_wire(buf, C(arr[i]));
    }
}

template<typename E, typename C = E>
void from_wire(Buffer& buf, shared_array<const void>& varr)
{
    Size slen;
    from_wire(buf, slen);
    shared_array<E> arr(slen.size);
    for(auto i : range(arr.size())) {
        C temp{};
        from_wire(buf, temp);
        arr[i] = temp;
    }
}
}

// serialize a field and all children (if Compound)
static
void to_wire_field(Buffer& buf, const FieldDesc* desc, const std::shared_ptr<const FieldStorage>& store)
{
    switch(store->code) {
    case StoreType::Null:
        switch(desc->code.code) {
        case TypeCode::Struct: {
            auto& top = *store->top;
            // serialize entire sub-structure
            for(auto off : range(desc->offset+1u, desc->next_offset)) {
                auto cdesc = desc + top.member_indicies[off];
                std::shared_ptr<const FieldStorage> cstore(store, store.get()+off); // TODO avoid shared_ptr/aliasing here
                if(cdesc->code!=TypeCode::Struct)
                    to_wire_field(buf, cdesc, cstore);
            }
        }
            return;
        default: break;
        }
        break;
    case StoreType::Real: {
        auto& fld = store->as<double>();
        switch(desc->code.code) {
        case TypeCode::Float32: to_wire(buf, float(fld)); return;
        case TypeCode::Float64: to_wire(buf, double(fld)); return;
        default: break;
        }
    }
        break;
    case StoreType::Integer: {
        auto& fld = store->as<int64_t>();
        switch(desc->code.code) {
        case TypeCode::Int8:  to_wire(buf, int8_t (fld)); return;
        case TypeCode::Int16: to_wire(buf, int16_t(fld)); return;
        case TypeCode::Int32: to_wire(buf, int32_t(fld)); return;
        case TypeCode::Int64: to_wire(buf, int64_t(fld)); return;
        default: break;
        }
    }
        break;
    case StoreType::UInteger: {
        auto& fld = store->as<uint64_t>();
        switch(desc->code.code) {
        case TypeCode::Bool:   to_wire(buf, uint8_t (fld!=0)); return;
        case TypeCode::UInt8:  to_wire(buf, uint8_t (fld)); return;
        case TypeCode::UInt16: to_wire(buf, uint16_t(fld)); return;
        case TypeCode::UInt32: to_wire(buf, uint32_t(fld)); return;
        case TypeCode::UInt64: to_wire(buf, uint64_t(fld)); return;
        default: break;
        }
    }
        break;
    case StoreType::String: {
        auto& fld = store->as<std::string>();
        switch(desc->code.code) {
        case TypeCode::String: to_wire(buf, fld); return;
        default: break;
        }
    }
        break;
    case StoreType::Compound: {
        auto& fld = store->as<Value>();
        switch (desc->code.code) {
        case TypeCode::Union:
            if(!fld) {
                // implied NULL Union member
                to_wire(buf, Size{size_t(-1)});

            } else {
                size_t index = 0u;
                for(auto& pair : desc->miter) {
                    if(Value::Helper::desc(fld)== desc+pair.second)
                        break;
                    index++;
                }
                if(index>=desc->miter.size())
                    throw std::logic_error("Union contains non-member type");
                to_wire(buf, Size{index});
                to_wire_full(buf, fld);
            }
            return;

        case TypeCode::Any:
            if(!fld) {
                to_wire(buf, uint8_t(0xff));

            } else {
                to_wire(buf, Value::Helper::desc(fld));
                to_wire_full(buf, fld);
            }
            return;
        default: break;
        }
    }
        break;
    case StoreType::Array: {
        auto& fld = store->as<shared_array<const void>>();
        switch (desc->code.code) {
        case TypeCode::BoolA:
            to_wire<bool, uint8_t>(buf, fld);
            return;
        case TypeCode::Int8:
        case TypeCode::UInt8:
            to_wire<uint8_t>(buf, fld);
            return;
        case TypeCode::Int16:
        case TypeCode::UInt16:
            to_wire<uint16_t>(buf, fld);
            return;
        case TypeCode::Int32:
        case TypeCode::UInt32:
        case TypeCode::Float32:
            to_wire<uint32_t>(buf, fld);
            return;
        case TypeCode::Int64:
        case TypeCode::UInt64:
        case TypeCode::Float64:
            to_wire<uint64_t>(buf, fld);
            return;
        case TypeCode::StringA:
            to_wire<std::string, const std::string&>(buf, fld);
            return;
        case TypeCode::StructA:{
            auto arr = fld.castTo<const Value>();
            to_wire(buf, Size{arr.size()});
            for(auto& elem : arr) {
                if(!elem) {
                    to_wire(buf, uint8_t(0u));
                } else {
                    to_wire(buf, uint8_t(1u));
                    assert(Value::Helper::desc(elem)==desc+1);
                    to_wire_full(buf, elem);
                }
            }
        }
            return;
        case TypeCode::UnionA: {
            auto arr = fld.castTo<const Value>();
            to_wire(buf, Size{arr.size()});
            for(auto& elem : arr) {
                if(!elem) {
                    to_wire(buf, uint8_t(0u));
                } else {
                    to_wire(buf, uint8_t(1u));

                    to_wire_full(buf, elem);
                }
            }
        }
            return;
        case TypeCode::AnyA:{
            auto arr = fld.castTo<const Value>();
            to_wire(buf, Size{arr.size()});
            for(auto& elem : arr) {
                if(!elem) {
                    to_wire(buf, uint8_t(0u));
                } else {
                    to_wire(buf, uint8_t(1u));

                    to_wire(buf, Value::Helper::desc(elem));
                    to_wire_full(buf, elem);
                }
            }
        }
            return;
        default: break;
        }
        break;
    }
    } // end case

    assert(false);
    buf.fault();
}

void to_wire_full(Buffer& buf, const Value& val)
{
    assert(!!val);

    to_wire_field(buf, Value::Helper::desc(val), Value::Helper::store(val));
}

void to_wire_valid(Buffer& buf, const Value& val)
{
    auto desc = Value::Helper::desc(val);
    auto store = Value::Helper::store(val);
    assert(!!desc);
    auto top = store->top;

    to_wire(buf, top->valid);
    top->valid.resize(top->members.size());

    // iterate marked fields
    for(auto bit = top->valid.findSet(desc->offset);
        bit<desc->next_offset;
        bit = top->valid.findSet(bit+1))
    {
        std::shared_ptr<const FieldStorage> cstore(store, store.get()+bit);
        to_wire_field(buf, desc + top->member_indicies[bit], cstore);
    }
}

namespace {
template<typename T>
T from_wire_as(Buffer& buf)
{
    T ret{};
    from_wire(buf, ret);
    return ret;
}
}

static
void from_wire_field(Buffer& buf, TypeStore& ctxt,  const FieldDesc* desc, const std::shared_ptr<FieldStorage>& store)
{
    switch(store->code) {
    case StoreType::Null:
        switch(desc->code.code) {
        case TypeCode::Struct: {
            auto& top = *store->top;
            // serialize entire sub-structure
            for(auto off : range(desc->offset+1u, desc->next_offset)) {
                auto cdesc = desc + top.member_indicies[off];
                std::shared_ptr<FieldStorage> cstore(store, store.get()+off); // TODO avoid shared_ptr/aliasing here
                if(cdesc->code!=TypeCode::Struct)
                    from_wire_field(buf, ctxt, cdesc, cstore);
            }
        }
            return;
        default: break;
        }
        break;
    case StoreType::Real: {
        auto& fld = store->as<double>();
        switch(desc->code.code) {
        case TypeCode::Float32: fld = from_wire_as<float>(buf); return;
        case TypeCode::Float64: fld = from_wire_as<double>(buf); return;
        default: break;
        }
    }
        break;
    case StoreType::Integer: {
        auto& fld = store->as<int64_t>();
        switch(desc->code.code) {
        case TypeCode::Int8:  fld = from_wire_as<int8_t>(buf); return;
        case TypeCode::Int16: fld = from_wire_as<int16_t>(buf); return;
        case TypeCode::Int32: fld = from_wire_as<int32_t>(buf); return;
        case TypeCode::Int64: fld = from_wire_as<int64_t>(buf); return;
        default: break;
        }
    }
        break;
    case StoreType::UInteger: {
        auto& fld = store->as<uint64_t>();
        switch(desc->code.code) {
        case TypeCode::Bool:   fld = 0!=from_wire_as<uint8_t>(buf); return;
        case TypeCode::UInt8:  fld = from_wire_as<int8_t>(buf); return;
        case TypeCode::UInt16: fld = from_wire_as<int16_t>(buf); return;
        case TypeCode::UInt32: fld = from_wire_as<int32_t>(buf); return;
        case TypeCode::UInt64: fld = from_wire_as<int64_t>(buf); return;
        default: break;
        }
    }
        break;
    case StoreType::String: {
        auto& fld = store->as<std::string>();
        switch(desc->code.code) {
        case TypeCode::String: from_wire(buf, fld); return;
        default: break;
        }
    }
        break;
    case StoreType::Compound: {
        auto& fld = store->as<Value>();
        switch (desc->code.code) {
        case TypeCode::Union: {
            Size select{};
            from_wire(buf, select);
            if(select.size==size_t(-1)) {
                fld = Value();
                return;

            } else if(select.size < desc->miter.size()) {
                std::shared_ptr<const FieldDesc> stype(store->top->desc,
                                                       desc + desc->miter[select.size].second); // alias
                fld = Value::Helper::build(stype, store, desc);

                from_wire_full(buf, ctxt, fld);
                return;
            }
        }
            break;

        case TypeCode::Any: {
            std::shared_ptr<std::vector<FieldDesc>> descs(new std::vector<FieldDesc>);
            TypeDeserContext dc{*descs, ctxt};

            from_wire(buf, dc);
            if(!buf.good())
                return;

            if(descs->empty()) {
                fld = Value();
                return;

            } else {
                FieldDesc_calculate_offset(descs->data());

                std::shared_ptr<const FieldDesc> stype(descs, descs->data()); // alias
                fld = Value::Helper::build(stype);

                from_wire_full(buf, ctxt, fld);
                return;

            }
        }
            break;

        default: break;
        }
    }
        break;
    case StoreType::Array: {
        auto& fld = store->as<shared_array<const void>>();
        switch (desc->code.code) {
        case TypeCode::BoolA:
            from_wire<bool, uint8_t>(buf, fld);
            return;
        case TypeCode::Int8:
        case TypeCode::UInt8:
            from_wire<uint8_t>(buf, fld);
            return;
        case TypeCode::Int16:
        case TypeCode::UInt16:
            from_wire<uint16_t>(buf, fld);
            return;
        case TypeCode::Int32:
        case TypeCode::UInt32:
        case TypeCode::Float32:
            from_wire<uint32_t>(buf, fld);
            return;
        case TypeCode::Int64:
        case TypeCode::UInt64:
        case TypeCode::Float64:
            from_wire<uint64_t>(buf, fld);
            return;
        case TypeCode::StringA:
            from_wire<std::string>(buf, fld);
            return;
        case TypeCode::StructA:{
            Size alen{};
            from_wire(buf, alen);
            shared_array<Value> arr(alen.size);
            std::shared_ptr<const FieldDesc> etype(store->top->desc,
                                                   desc + 1); // alias
            for(auto& elem : arr) {
                if(from_wire_as<uint8_t>(buf)!=0) { // strictly 1 or 0
                    elem = Value::Helper::build(etype, store, desc);

                    from_wire_full(buf, ctxt, elem);
                }
            }

            fld = arr.freeze().castTo<const void>();
        }
            return;
        case TypeCode::UnionA: {
            Size alen{};
            from_wire(buf, alen);
            shared_array<Value> arr(alen.size);
            auto cdesc = desc+1;

            for(auto& elem : arr) {
                if(from_wire_as<uint8_t>(buf)!=0) { // strictly 1 or 0
                    Size select{};
                    from_wire(buf, select);

                    if(select.size==size_t(-1)) {
                        // null element.  treated the same as 0 case (which is what actually happens)

                    } else if(select.size < cdesc->miter.size()) {
                        std::shared_ptr<const FieldDesc> stype(store->top->desc,
                                                               cdesc + cdesc->miter[select.size].second); // alias
                        elem = Value::Helper::build(stype, store, desc);

                        from_wire_full(buf, ctxt, elem);

                    } else {
                        // invalid selector
                        buf.fault();
                        return;
                    }
                }
            }

            fld = arr.freeze().castTo<const void>();
        }
            return;
        case TypeCode::AnyA:{
            Size alen{};
            from_wire(buf, alen);
            shared_array<Value> arr(alen.size);

            for(auto& elem : arr) {
                if(from_wire_as<uint8_t>(buf)!=0) { // strictly 1 or 0
                    std::shared_ptr<std::vector<FieldDesc>> descs(new std::vector<FieldDesc>);
                    TypeDeserContext dc{*descs, ctxt};

                    from_wire(buf, dc);
                    if(!buf.good())
                        return;

                    if(!descs->empty()) {
                        FieldDesc_calculate_offset(descs->data());

                        std::shared_ptr<const FieldDesc> stype(descs, descs->data()); // alias
                        elem = Value::Helper::build(stype, store, desc);

                        from_wire_full(buf, ctxt, elem);
                    }
                }
            }

            fld = arr.freeze().castTo<const void>();
        }
            return;
        default: break;
        }
        break;

    }
    } // end case

    buf.fault();
}

void from_wire_full(Buffer& buf, TypeStore& ctxt, Value& val)
{
    assert(!!val);

    from_wire_field(buf, ctxt, Value::Helper::desc(val), Value::Helper::store(val));
}

void from_wire_valid(Buffer& buf, TypeStore& ctxt, Value& val)
{
    auto desc = Value::Helper::desc(val);
    auto store = Value::Helper::store(val);
    assert(!!desc);
    auto top = store->top;

    from_wire(buf, top->valid);
    // encoding rounds # of bits to whole bytes, so we may trim
    top->valid.resize(top->members.size());
    if(!buf.good())
        return;

    for(auto bit = top->valid.findSet(desc->offset);
        bit<desc->next_offset;
        bit = top->valid.findSet(bit+1))
    {
        std::shared_ptr<FieldStorage> cstore(store, store.get()+bit);
        from_wire_field(buf, ctxt, desc + top->member_indicies[bit], cstore);
    }
}

void from_wire_type_value(Buffer& buf, TypeStore& ctxt, Value& val)
{
    std::shared_ptr<std::vector<FieldDesc>> descs(new std::vector<FieldDesc>);
    TypeDeserContext dc{*descs, ctxt};

    from_wire(buf, dc);
    if(!buf.good())
        return;

    if(!descs->empty()) {
        FieldDesc_calculate_offset(descs->data());

        std::shared_ptr<const FieldDesc> stype(descs, descs->data()); // alias
        val = Value::Helper::build(stype);

        from_wire_full(buf, ctxt, val);

    } else {
        val = Value();
    }
}

}} // namespace pvxs::impl

#endif // DATAENCODE_H