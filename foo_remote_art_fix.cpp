// foo_remote_art_fix.cpp
//
// Remote Art Fix - foobar2000 组件
//
// 为什么需要它：
//   核心的封面管线（album_art_manager_v2、imageLoader 的 "trackimage://"）
//   对远程文件（filesystem::is_remote() == true）故意不读取内嵌封面。
//   这个限制是为了避免 UI 的高频批量查询去下载大量远程数据，但副作用是：
//   依赖这些管线的主题/组件在 WebDAV、HTTP(S) 直链等网络音源上显示不出封面——
//   即使封面就嵌在文件里。
//
// 它做什么：
//   注册一个 album_art_fallback（封面管理器在所有常规来源都失败后才会询问的
//   最后一环），分两条通道读取文件的内嵌封面：
//     - 文件系统通道：WebDAV 等可 seek 的文件系统，沿用核心提取器（提示为 null）；
//     - HTTP(S) 通道：用 http_client 打开直链（支持 Range），把可 seek 的 file
//       作为 p_filehint 交给核心提取器。
//   共同点：都以一次 album_art_extractor::g_open() 读取，不手写格式解析、
//   不访问任何在线封面服务。
//
// 安装：
//   把 DLL 放进 <profile>/user-components/ 后重启 foobar2000。

#include <SDK/foobar2000.h>
#include <SDK/album_art.h>
#include <SDK/album_art_helpers.h>
#include <SDK/filesystem.h>
#include <SDK/http_client.h>
#include <SDK/component.h>
#include <SDK/input.h>
#include <cstring>
#include <vector>

DECLARE_COMPONENT_VERSION(
    "Remote Art Fix",
    "1.1.1",
    "为依赖核心封面管线的主题/组件提取远程（WebDAV / HTTP(S)）曲目的内嵌封面。"
);
VALIDATE_COMPONENT_FILENAME("foo_remote_art_fix.dll");

namespace {

bool starts_with_ci(const char* s, const char* prefix) {
    while (*prefix) {
        char a = *s++;
        if (!a) return false;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool is_http_url(const char* path) {
    return path && (starts_with_ci(path, "http://") || starts_with_ci(path, "https://"));
}

//! 取 URL/路径的扩展名（小写，忽略 ?query 与 #fragment）；非法或没有则返回空。
pfc::string8 url_extension(const char* url) {
    if (!url) return pfc::string8();
    const char* end = url;
    while (*end && *end != '?' && *end != '#') ++end;
    const char* dot = nullptr;
    for (const char* p = url; p < end; ++p) {
        if (*p == '.') dot = p;
    }
    if (!dot || dot + 1 >= end) return pfc::string8();
    pfc::string8 ext;
    for (const char* p = dot + 1; p < end; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return pfc::string8();
        ext.add_string(&c, 1);
    }
    return ext;
}

pfc::string8 synthetic_path(const pfc::string8& ext) {
    pfc::string8 path = "remote_art.";
    path += ext;
    return path;
}

bool is_supported_ext(const pfc::string8& ext) {
    if (ext.is_empty()) return false;
    return album_art_extractor::g_is_supported_path(synthetic_path(ext).c_str());
}

//! Content-Type → 扩展名。
pfc::string8 extension_from_content_type(const char* ct) {
    if (!ct) return pfc::string8();
    pfc::string8 type;
    for (const char* p = ct; *p && *p != ';'; ++p) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        type.add_string(&c, 1);
    }
    if (type == "audio/flac" || type == "audio/x-flac") return "flac";
    if (type == "audio/mpeg" || type == "audio/mp3" || type == "audio/x-mp3") return "mp3";
    if (type == "audio/mp4" || type == "audio/x-m4a" || type == "audio/m4a") return "m4a";
    if (type == "audio/ogg" || type == "application/ogg" || type == "audio/vorbis") return "ogg";
    if (type == "audio/opus" || type == "audio/x-opus") return "opus";
    if (type == "audio/x-ape" || type == "audio/ape") return "ape";
    if (type == "audio/x-wavpack" || type == "audio/wavpack") return "wv";
    if (type == "audio/x-musepack" || type == "audio/musepack") return "mpc";
    if (type == "audio/x-tta") return "tta";
    if (type == "audio/wav" || type == "audio/x-wav" || type == "audio/wave") return "wav";
    if (type == "audio/aiff" || type == "audio/x-aiff") return "aiff";
    if (type == "audio/x-ms-wma" || type == "video/x-ms-asf") return "wma";
    if (type == "audio/x-dsf" || type == "audio/dsf") return "dsf";
    return pfc::string8();
}

//! 文件头魔数 → 扩展名。
pfc::string8 extension_from_magic(const uint8_t* b, t_size n) {
    auto has = [b, n](t_size off, const char* sig, t_size len) {
        return n >= off + len && memcmp(b + off, sig, len) == 0;
    };
    if (has(0, "fLaC", 4)) return "flac";
    if (has(0, "ID3", 3)) return "mp3";
    if (has(0, "OggS", 4)) return "ogg";
    if (has(4, "ftyp", 4)) return "m4a";
    if (has(0, "MAC ", 4)) return "ape";
    if (has(0, "wvpk", 4)) return "wv";
    if (has(0, "MPCK", 4) || has(0, "MP+", 3)) return "mpc";
    if (has(0, "TTA1", 4)) return "tta";
    if (has(0, "RIFF", 4) && has(8, "WAVE", 4)) return "wav";
    if (has(0, "FORM", 4) && (has(8, "AIFF", 4) || has(8, "AIFC", 4))) return "aiff";
    if (has(0, "DSD ", 4)) return "dsf";
    if (has(0, "FRM8", 4)) return "dff";
    static const uint8_t asf[] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                  0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
    if (n >= sizeof(asf) && memcmp(b, asf, sizeof(asf)) == 0) return "wma";
    return pfc::string8();
}

//! 依次用 URL 扩展名、Content-Type、文件头魔数确定提取器分派扩展名。
pfc::string8 resolve_extension(const char* path, const file::ptr& f, abort_callback& abort) {
    pfc::string8 ext = url_extension(path);
    if (is_supported_ext(ext)) return ext;

    if (!f.is_valid()) return pfc::string8();

    pfc::string8 ct;
    if (f->get_content_type(ct)) {
        ext = extension_from_content_type(ct.c_str());
        if (is_supported_ext(ext)) return ext;
    }

    if (f->can_seek()) {
        uint8_t buf[64];
        t_size got = 0;
        try {
            got = f->read(buf, sizeof(buf), abort);
        } catch (exception_aborted const&) {
            throw;
        } catch (...) {
            got = 0;
        }
        try {
            f->seek(0, abort);
        } catch (exception_aborted const&) {
            throw;
        } catch (...) {
            return pfc::string8(); // 已经读走一段又回不到开头，不能交给提取器
        }
        if (got) {
            ext = extension_from_magic(buf, got);
            if (is_supported_ext(ext)) return ext;
        }
    }
    return pfc::string8();
}

file::ptr open_http(const char* url, abort_callback& abort) {
    auto request = http_client::get()->create_request("GET");
    return request->run(url, abort);
}

struct found_art_t {
    GUID id;
    album_art_data_ptr data;
};

//! 打开封面实例并取出请求的封面类型。
//! 标准 g_open 取不到时，回退为直接调用已启用输入（不经过输入包装层）。
album_art_extractor_instance_ptr open_art(file::ptr hint, const char* path,
                                           pfc::list_base_const_t<GUID> const& ids,
                                           abort_callback& abort,
                                           std::vector<found_art_t>& found)
{
    auto probe = [&](album_art_extractor_instance_ptr const& inst) {
        bool got = false;
        for (t_size i = 0; i < ids.get_count(); ++i) {
            try {
                album_art_data_ptr data = inst->query(ids[i], abort);
                if (data.is_valid()) {
                    found_art_t f;
                    f.id = ids[i];
                    f.data = data;
                    found.push_back(f);
                    got = true;
                }
            } catch (exception_aborted const&) {
                throw;
            } catch (...) {
            }
        }
        return got || ids.get_count() == 0;
    };

    try {
        auto inst = album_art_extractor::g_open(hint, path, abort);
        if (probe(inst)) return inst;
    } catch (exception_aborted const&) {
        throw;
    } catch (...) {
    }

    pfc::list_t<input_entry::ptr> entries;
    input_manager_v3::ptr mgr;
    if (fb2k::std_api_try_get(mgr)) {
        mgr->get_enabled_inputs(entries);
    } else {
        service_enum_t<input_entry> e;
        input_entry::ptr p;
        while (e.next(p)) entries.add_item(p);
    }

    const char* ext = pfc::string_extension(path);
    for (t_size i = 0; i < entries.get_count(); ++i) {
        input_entry::ptr entry = entries[i];
        if (!entry->is_our_path(path, ext)) continue;

        // 同一个 hint 会被多个输入复用，每次尝试前都要回到开头。
        if (hint.is_valid()) {
            if (!hint->can_seek()) break;
            try {
                hint->seek(0, abort);
            } catch (exception_aborted const&) {
                throw;
            } catch (...) {
                break;
            }
        }

        try {
            album_art_extractor_instance_ptr inst;
            inst ^= entry->open(album_art_extractor_instance::class_guid, hint, path, nullptr, abort);
            if (inst.is_valid() && probe(inst)) return inst;
        } catch (exception_aborted const&) {
            throw;
        } catch (...) {
        }
    }
    return album_art_extractor_instance_ptr();
}

//! 单个远程曲目的封面实例。内部只打开一次提取器；已查询的条目会缓存。
class remote_art_instance : public album_art_extractor_instance_v2 {
public:
    remote_art_instance(pfc::string8 path, album_art_extractor_instance_ptr inner, file::ptr source)
        : m_path(path), m_inner(inner), m_source(source) {}

    album_art_data_ptr query(const GUID& art_id, abort_callback& abort) override {
        album_art_data_ptr cached;
        if (m_entries.query(art_id, cached)) return cached;
        album_art_data_ptr data = m_inner->query(art_id, abort);
        m_entries.set(art_id, data);
        return data;
    }

    album_art_path_list::ptr query_paths(const GUID&, abort_callback&) override {
        // 内嵌封面：数据来源就是曲目文件本身（http 直链时为 URL）
        return new service_impl_t<album_art_path_list_impl>(m_path.c_str());
    }

    void cache(const GUID& id, album_art_data_ptr data) {
        m_entries.set(id, data);
    }

private:
    pfc::string8 m_path;
    album_art_extractor_instance_ptr m_inner;
    file::ptr m_source; // 保持数据源存活（提取器可能延迟读取）
    pfc::map_t<GUID, album_art_data_ptr> m_entries;
};

//! 注册到核心的兜底实现，只处理远程文件。
class remote_art_fallback : public album_art_fallback {
public:
    album_art_extractor_instance_v2::ptr open(
        metadb_handle_list_cref items,
        pfc::list_base_const_t<GUID> const& art_ids,
        abort_callback& abort) override
    {
        if (items.get_count() == 0) throw exception_album_art_not_found();

        const char* path = items[0]->get_path();
        if (!path || !*path) throw exception_album_art_not_found();

        const bool http = is_http_url(path);
        if (!http && !filesystem::g_is_remote_or_unrecognized(path)) {
            throw exception_album_art_not_found(); // 本地文件不插手
        }

        // 判断是否应走文件系统通道：非 http 一定走；http(s) 需要该路径被某个
        // 文件系统服务认领、且能打开成可 seek 的文件（例如 WebDAV）。
        bool fs_like = !http;
        if (http) {
            try {
                auto fs = filesystem::tryGet(path);
                if (fs.is_valid()) {
                    file::ptr probe;
                    filesystem::g_open_read(probe, path, abort);
                    fs_like = probe->can_seek();
                }
            } catch (exception_aborted const&) {
                throw;
            } catch (...) {
                fs_like = false;
            }
        }

        if (fs_like) {
            try {
                std::vector<found_art_t> found;
                auto inner = open_art(file::ptr(), path, art_ids, abort, found);
                if (inner.is_valid()) {
                    auto* inst = new service_impl_t<remote_art_instance>(path, inner, file::ptr());
                    for (auto& f : found) inst->cache(f.id, f.data);
                    return inst;
                }
            } catch (exception_aborted const&) {
                throw;
            } catch (pfc::exception const&) {
                // 非 http 失败就是没封面；http(s) 可能是该文件系统服务不可用，
                // 继续尝试下面的 http_client 通道。
                if (!http) throw exception_album_art_not_found();
            }
            if (!http) throw exception_album_art_not_found();
        }

        if (!http) throw exception_album_art_not_found();

        try {
            file::ptr source = open_http(path, abort);
            pfc::string8 ext = resolve_extension(path, source, abort);
            if (ext.is_empty()) throw exception_album_art_not_found();

            std::vector<found_art_t> found;
            auto inner = open_art(source, synthetic_path(ext).c_str(), art_ids, abort, found);
            if (!inner.is_valid()) throw exception_album_art_not_found();

            auto* inst = new service_impl_t<remote_art_instance>(path, inner, source);
            for (auto& f : found) inst->cache(f.id, f.data);
            return inst;
        } catch (exception_aborted const&) {
            throw;
        } catch (pfc::exception const&) {
            throw exception_album_art_not_found();
        }
    }
};

FB2K_SERVICE_FACTORY(remote_art_fallback);

} // namespace
