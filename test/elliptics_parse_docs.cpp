#include "similarity.hpp"
#include "simdoc.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>

using namespace ioremap;
using namespace ioremap::similarity;

class loader {
	public:
		loader(const std::string &remote, const std::string &group_string, const std::string &log, int level) :
		m_logger(log.c_str(), level),
		m_node(m_logger) {
			m_node.add_remote(remote.c_str());

			struct digitizer {
				int operator() (const std::string &str) {
					return atoi(str.c_str());
				}
			};

			std::vector<std::string> gr;
			boost::split(gr, group_string, boost::is_any_of(":"));

			std::transform(gr.begin(), gr.end(), std::back_inserter<std::vector<int>>(m_groups), digitizer());
		}

		void load(const std::string &index, const std::string &input_dir, const std::string &learn_file) {
			std::ifstream in(learn_file.c_str());

			m_indexes.clear();
			m_indexes.push_back(index);
			m_input_dir = input_dir;

			std::set<int> ids;

			std::string line;
			int line_num = 0;
			while (std::getline(in, line)) {
				if (!in.good())
					break;

				line_num++;

				int doc[2];

				int num = sscanf(line.c_str(), "%d\t%d\t", &doc[0], &doc[1]);
				if (num != 2) {
					fprintf(stderr, "failed to parse string: %d, tokens found: %d\n", line_num, num);
					break;
				}

				const char *pos = strrchr(line.c_str(), '\t');
				if (!pos) {
					fprintf(stderr, "could not find last tab delimiter\n");
					break;
				}

				pos++;
				if (pos && *pos) {
					learn_element le;

					le.doc_ids = std::vector<int>(doc, doc+2);
					le.request.assign(pos);

					ids.insert(doc[0]);
					ids.insert(doc[1]);
				}
			}

			m_doc_ids.assign(ids.begin(), ids.end());

			int num = std::thread::hardware_concurrency();
			if (num == 0)
				num = sysconf(_SC_NPROCESSORS_ONLN);

			add_documents(num);
		}

	private:
		std::vector<int> m_doc_ids;

		std::vector<std::string> m_indexes;
		std::string m_input_dir;

		elliptics::file_logger m_logger;
		elliptics::node m_node;
		std::vector<int> m_groups;

		struct doc_thread {
			static const int max_pending = 10;
			int id;
			int step;

			std::atomic_int pending;
			std::mutex lock;
			std::condition_variable cond;

			elliptics::session session;

			doc_thread(elliptics::node &node) : session(node) {}
		};

		void load_documents(std::shared_ptr<doc_thread> dth) {
			document_parser parser;

			dth->session.set_groups(m_groups);
			dth->session.set_exceptions_policy(elliptics::session::no_exceptions);
			dth->session.set_ioflags(DNET_IO_FLAGS_CACHE);

			for (size_t i = dth->id; i < m_doc_ids.size(); i += dth->step) {
				simdoc doc;
				doc.id = m_doc_ids[i];

				std::string doc_id_str = lexical_cast(doc.id);
				std::string file = m_input_dir + doc_id_str + ".html";
				try {
					parser.feed(file.c_str(), "");

					doc.text = parser.text();
					parser.generate_ngrams(doc.text, doc.ngrams);

					msgpack::sbuffer buffer;
					msgpack::pack(&buffer, doc);

					dth->session.write_data(doc_id_str, elliptics::data_pointer::copy(buffer.data(), buffer.size()), 0)
						.connect(std::bind(&loader::update_index, this, dth, doc_id_str));

					dth->pending++;

					if (dth->pending > doc_thread::max_pending) {
						std::cout << "going to sleep: " << dth->pending << std::endl;
						std::unique_lock<std::mutex> guard(dth->lock);
						dth->cond.wait(guard);
						std::cout << "going to sleep: " << dth->pending << std::endl;
					}

				} catch (const std::exception &e) {
					std::cerr << file << ": caught exception: " << e.what() << std::endl;
				}
			}
		}

		void update_index(std::shared_ptr<doc_thread> dth, const std::string &doc_id_str) {
			std::vector<elliptics::data_pointer> tmp;
			tmp.resize(m_indexes.size());
			dth->session.set_indexes(doc_id_str, m_indexes, tmp)
				.connect(std::bind(&loader::update_index_completion, this, dth));
		}

		void update_index_completion(std::shared_ptr<doc_thread> dth) {
			dth->pending--;
			if (dth->pending < doc_thread::max_pending / 2)
				dth->cond.notify_one();
		}

		void add_documents(int cpunum) {
			std::vector<std::thread> threads;

			wookie::timer tm;

			for (int i = 0; i < cpunum; ++i) {
				std::shared_ptr<doc_thread> dth = std::make_shared<doc_thread>(m_node);

				dth->id = i;
				dth->step = cpunum;
				dth->pending = 0;
				dth->session = elliptics::session(m_node);

				threads.emplace_back(std::bind(&loader::load_documents, this, dth));
			}

			for (int i = 0; i < cpunum; ++i) {
				threads[i].join();
			}

			threads.clear();
			long docs_loading_time = tm.restart();

			printf("documents: %zd, load-time: %ld msec\n", m_doc_ids.size(), docs_loading_time);
		}
};

int main(int argc, char *argv[])
{
	namespace bpo = boost::program_options;

	bpo::options_description generic("Similarity options");

	std::string input_dir, pairs, index;
	std::string remote, group_string;
	std::string log_file;
	int log_level;

	generic.add_options()
		("help", "This help message")
		("input-dir", bpo::value<std::string>(&input_dir)->required(), "Input directory")
		("pairs", bpo::value<std::string>(&pairs)->required(), "Pairs data file")
		("index", bpo::value<std::string>(&index)->required(), "Elliptics index for loaded objects")

		("remote", bpo::value<std::string>(&remote)->required(), "Remote elliptics server")
		("groups", bpo::value<std::string>(&group_string)->required(), "Colon separated list of groups")
		("log", bpo::value<std::string>(&log_file)->default_value("/dev/stdout"), "Elliptics log file")
		("log-level", bpo::value<int>(&log_level)->default_value(DNET_LOG_INFO), "Elliptics log-level")
		;

	bpo::variables_map vm;

	try {
		bpo::store(bpo::parse_command_line(argc, argv, generic), vm);
		if (vm.count("help")) {
			std::cout << generic << std::endl;
			return 0;
		}

		bpo::notify(vm);
	} catch (const std::exception &e) {
		std::cerr << "Invalid options: " << e.what() << "\n" << generic << std::endl;
		return -1;
	}

	try {
		loader el(remote, group_string, log_file, log_level);
		el.load(index, input_dir, pairs);

	} catch (const std::exception &e) {
		std::cerr << "exception: " << e.what() << std::endl;
		return -1;
	}
}