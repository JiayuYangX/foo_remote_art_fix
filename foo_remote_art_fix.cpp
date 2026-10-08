// foo_remote_art_fix.cpp
//
// Remote Art Fix - foobar2000 组件
//
// 为什么需要它：
//   核心的封面管线（album_art_manager_v2、imageLoader 的 "trackimage://"）
//   对远程文件（filesystem::is_remote() == true）故意不读取内嵌封面。
//   这个限制是为了避免 UI 的高频批量查询去下载大量远程数据，但副作用是：
//   依赖这些管线的主题/组件（如 SMP/JSplitter 主题、WebView2 UI 等）在
//   WebDAV 等网络音源上显示不出封面——即使封面就嵌在文件里、属性面板能看见。
//
// 它做什么：
//   注册一个 album_art_fallback（封面管理器在所有常规来源都失败后才会询问的
//   最后一环），用 album_art_extractor 直接提取远程文件的内嵌封面，绕过上述限制。
//
// 安装：
//   把 DLL 放进 <profile>/user-components/ 后重启 foobar2000。

#include <SDK/foobar2000.h>
#include <SDK/album_art.h>
#include <SDK/album_art_helpers.h>
#include <SDK/filesystem.h>
#include <SDK/component.h>

DECLARE_COMPONENT_VERSION(
    "Remote Art Fix",
    "1.0.0",
    "为依赖核心封面管线的主题/组件提取远程（WebDAV/网络）曲目的内嵌封面。"
);
VALIDATE_COMPONENT_FILENAME("foo_remote_art_fix.dll");

namespace {

//! 从单个文件提取内嵌封面——即核心管线对远程文件跳过的那一步操作。
//! 除用户中止外，所有提取器异常统一转成“未找到封面”。
album_art_data_ptr extract_embedded_artwork(const char* path, const GUID& art_id, abort_callback& abort) {
    try {
        auto extractor = album_art_extractor::g_open(nullptr, path, abort);
        return extractor->query(art_id, abort);
    } catch (exception_aborted const&) {
        throw; // 用户中止不能吞掉
    } catch (pfc::exception const&) {
        throw exception_album_art_not_found();
    }
}

//! 单个远程曲目的封面实例。已提取的条目会缓存，之后 query() 不会再走网络。
class remote_art_instance : public album_art_extractor_instance_v2 {
public:
    explicit remote_art_instance(const char* path) : m_path(path) {}

    void add(const GUID& art_id, album_art_data_ptr data) {
        m_entries.set(art_id, data);
    }

    album_art_data_ptr query(const GUID& art_id, abort_callback& abort) override {
        album_art_data_ptr cached;
        if (m_entries.query(art_id, cached)) return cached;             // 命中缓存
        return extract_embedded_artwork(m_path.c_str(), art_id, abort); // 兜底现取
    }

    album_art_path_list::ptr query_paths(const GUID&, abort_callback&) override {
        // 内嵌封面：数据来源就是曲目文件本身
        return new service_impl_t<album_art_path_list_impl>(m_path.c_str());
    }

private:
    pfc::string8 m_path;
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

        // items 按专辑分组传入，取第一个即可
        const char* path = items[0]->get_path();

        // 本地文件不插手：核心自己处理得很好
        if (!filesystem::g_is_remote_or_unrecognized(path)) throw exception_album_art_not_found();

        // 先探测请求的图类型，保留第一个能读到的；
        // 紧接着的 query() 就能直接走缓存，不会二次下载。
        GUID found_id = pfc::guid_null;
        album_art_data_ptr found_data;
        for (t_size i = 0; i < art_ids.get_count(); ++i) {
            try {
                found_data = extract_embedded_artwork(path, art_ids[i], abort);
                found_id = art_ids[i];
                break;
            } catch (exception_aborted const&) {
                throw;
            } catch (pfc::exception const&) {
                // 换下一个图类型继续试
            }
        }
        if (!found_data.is_valid()) throw exception_album_art_not_found();

        auto instance = new service_impl_t<remote_art_instance>(path);
        instance->add(found_id, found_data);
        return instance;
    }
};

FB2K_SERVICE_FACTORY(remote_art_fallback);

} // namespace
