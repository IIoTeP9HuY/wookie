#ifndef __WOOKIE_STORAGE_HPP
#define __WOOKIE_STORAGE_HPP

#include "split.hpp"
#include "index_data.hpp"

#include <elliptics/session.hpp>

#include <fstream>

namespace ioremap { namespace wookie {

class storage : public elliptics::node {
	public:
		explicit storage(elliptics::logger &log, const std::string &ns);

		void set_groups(const std::vector<int> groups);

		std::vector<elliptics::find_indexes_result_entry> find(const std::vector<std::string> &indexes);
		std::vector<elliptics::find_indexes_result_entry> find(const std::vector<dnet_raw_id> &indexes);

		elliptics::async_write_result write_document(ioremap::wookie::document &d);
		elliptics::async_read_result read_data(const elliptics::key &key);

		std::vector<dnet_raw_id> transform_tokens(const std::vector<std::string> &tokens);

		elliptics::session create_session(void);

	private:
		std::vector<int> m_groups;
		wookie::split m_spl;
		std::string m_namespace;
};

}}

#endif /* __WOOKIE_STORAGE_HPP */
