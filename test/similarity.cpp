#include "wookie/parser.hpp"
#include "wookie/lexical_cast.hpp"

#include <algorithm>
#include <fstream>
#include <list>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <iconv.h>
#include <string.h>

#include <boost/locale.hpp>
#include <boost/program_options.hpp>

using namespace ioremap;

class charset_convert {
	public:
		charset_convert(const char *to, const char *from) {
			m_tmp.resize(128);

			m_iconv = iconv_open(to, from);
			if (m_iconv == (iconv_t)-1) {
				int err = errno;
				std::ostringstream ss;
				ss << "invalid conversion: " <<
					from << " -> " << to << " : " << strerror(err) << " [" << err << "]";
				throw std::runtime_error(ss.str());
			}
		}

		~charset_convert() {
			iconv_close(m_iconv);
		}

		void reset(void) {
			::iconv(m_iconv, NULL, NULL, NULL, NULL);
		}

		std::string convert(const std::string &in) {
			char *src = const_cast<char *>(&in[0]);
			size_t inleft = in.size();

			std::ostringstream out;

			while (inleft > 0) {
				char *dst = const_cast<char *>(&m_tmp[0]);
				size_t outleft = m_tmp.size();

				size_t size = ::iconv(m_iconv, &src, &inleft, &dst, &outleft);

				if (size == (size_t)-1) {
					if (errno == EINVAL)
						break;
					if (errno == E2BIG)
						continue;
					if (errno == EILSEQ) {
						src++;
						inleft--;
						continue;
					}
				}

				out.write(m_tmp.c_str(), m_tmp.size() - outleft);
			}

			return out.str();
		}

	private:
		iconv_t m_iconv;
		std::string m_tmp;
};

class document_parser {
	public:
		document_parser() : m_loc(m_gen("en_UR.UTF8")) {
		}

		void feed(const char *path) {
			std::ifstream in(path);
			if (in.bad())
				return;

			std::ostringstream ss;

			ss << in.rdbuf();

			m_parser.parse(ss.str(), "utf8");
		}

		std::string text(void) const {
			return m_parser.text(" ");
		}

		void generate(const std::string &text, int ngram_num, std::vector<long> &hashes) {
			namespace lb = boost::locale::boundary;
			lb::ssegment_index wmap(lb::word, text.begin(), text.end(), m_loc);
			wmap.rule(lb::word_any);

			std::list<std::string> ngram;

			for (auto it = wmap.begin(), e = wmap.end(); it != e; ++it) {
				std::string token = boost::locale::to_lower(it->str(), m_loc);

				ngram.push_back(token);

				if ((int)ngram.size() == ngram_num) {
					std::ostringstream ss;
					std::copy(ngram.begin(), ngram.end(), std::ostream_iterator<std::string>(ss, ""));

					hashes.emplace_back(hash(ss.str(), 0));
					ngram.pop_front();
				}
			}

			std::set<long> tmp(hashes.begin(), hashes.end());
			hashes.assign(tmp.begin(), tmp.end());
		}

	private:
		wookie::parser m_parser;
		boost::locale::generator m_gen;
		std::locale m_loc;

		// murmur hash
		long hash(const std::string &str, long seed) const {
			const uint64_t m = 0xc6a4a7935bd1e995LLU;
			const int r = 47;

			long h = seed ^ (str.size() * m);

			const uint64_t *data = (const uint64_t *)str.data();
			const uint64_t *end = data + (str.size() / 8);

			while (data != end) {
				uint64_t k = *data++;

				k *= m;
				k ^= k >> r;
				k *= m;

				h ^= k;
				h *= m;
			}

			const unsigned char *data2 = (const unsigned char *)data;

			switch (str.size() & 7) {
			case 7: h ^= (uint64_t)data2[6] << 48;
			case 6: h ^= (uint64_t)data2[5] << 40;
			case 5: h ^= (uint64_t)data2[4] << 32;
			case 4: h ^= (uint64_t)data2[3] << 24;
			case 3: h ^= (uint64_t)data2[2] << 16;
			case 2: h ^= (uint64_t)data2[1] << 8;
			case 1: h ^= (uint64_t)data2[0];
				h *= m;
			};

			h ^= h >> r;
			h *= m;
			h ^= h >> r;

			return h;
		}
};

struct ngram {
	ngram(std::vector<long> &h) {
		hashes.swap(h);
	}

	ngram(void) {}

	std::vector<long> hashes;
};

class document {
	public:
		document(const char *path) : m_path(path) {
		}

		const std::string &name(void) const {
			return m_path;
		}

		std::vector<ngram> &ngrams(void) {
			return m_ngrams;
		}

	private:
		std::string m_path;
		std::vector<ngram> m_ngrams;
};

enum feature_bits {
	ngram2_match = 0,
	ngram3_match,
	ngram4_match,
	ngram5_match,
	ngram6_match,

	ngram2_req_match,
	ngram3_req_match,
	ngram4_req_match,
	ngram5_req_match,
	ngram6_req_match,

	feature_num
};

struct learn_element {
	learn_element() : valid(true) {
		memset(features, 0, sizeof(features));
	}

	std::vector<int> docs;
	std::string request;
	bool valid;

	int features[feature_num];
};

class learner {
	public:
		learner(const std::string &input, const std::string &learn_file) : m_input(input) {
			std::ifstream in(learn_file.c_str());

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
					continue;
				}

				const char *pos = strrchr(line.c_str(), '\t');
				if (!pos) {
					fprintf(stderr, "could not find last tab delimiter\n");
					continue;
				}

				pos++;
				if (pos && *pos) {
					learn_element le;

					le.docs = std::vector<int>(doc, doc+2);
					le.request.assign(pos);

					m_elements.emplace_back(std::move(le));
				}
			}

			printf("pairs loaded: %zd\n", m_elements.size());
			add_documents(8);
		}

	private:
		std::string m_input;
		std::vector<learn_element> m_elements;

		struct doc_thread {
			int id;
			int step;
		};

		void generate_ngrams(document_parser &parser, const std::string &text, std::vector<ngram> &ngrams) {
			for (int i = 1; i <= 4; ++i) {
				std::vector<long> hashes;

				parser.generate(text, i, hashes);

				ngrams.emplace_back(hashes);
			}
		}

		void load_documents(struct doc_thread &dth) {
			document_parser parser;

			for (size_t i = dth.id; i < m_elements.size(); i += dth.step) {
				learn_element &le = m_elements[i];

				std::vector<ngram> req_ngrams; 
				generate_ngrams(parser, le.request, req_ngrams);

				std::map<int, document> local_docs;

				for (auto doc_id : le.docs) {
					auto it = local_docs.find(doc_id);
					if (it != local_docs.end())
						continue;

					std::string file = m_input + lexical_cast(doc_id) + ".html";
					try {
						parser.feed(file.c_str());
						std::string text = parser.text();

						document doc(file.c_str());
						generate_ngrams(parser, text, doc.ngrams());

						local_docs.emplace(doc_id, doc);
					} catch (const std::exception &e) {
						std::cerr << file << ": caught exception: " << e.what() << std::endl;
						le.valid = false;
						break;
					}
				}

				generate_features(le, req_ngrams, local_docs);
			}
		}

		ngram intersect(const ngram &f, const ngram &s) {
			ngram tmp;
			tmp.hashes.resize(f.hashes.size());

			auto inter = std::set_intersection(f.hashes.begin(), f.hashes.end(),
					s.hashes.begin(), s.hashes.end(), tmp.hashes.begin());
			tmp.hashes.resize(inter - tmp.hashes.begin());

			return tmp;
		}

		void generate_features(learn_element &le, const std::vector<ngram> &req_ngrams, std::map<int, document> &docs) {
			std::vector<ngram> matched;

			for (size_t i = 0; i < docs.begin()->second.ngrams().size(); ++i) {
				ngram out;

				for (auto it = docs.begin(); it != docs.end(); ++it) {
					if ((i == 0) && (it->second.ngrams()[0].hashes.size() == 0)) {
						le.valid = false;
						return;
					}

					if (it == docs.begin()) {
						out = it->second.ngrams()[i];
					} else {
						ngram &s = it->second.ngrams()[i];
						out = intersect(out, s);
					}
				}

				matched.emplace_back(out);
			}

			std::vector<ngram> matched_req;
			for (size_t i = 0; i < req_ngrams.size(); ++i) {
				ngram out;

				const ngram &f = req_ngrams[i];
				const ngram &s = matched[i];

				out = intersect(f, s);

				printf("%zd/%zd: '%s': intersected ngrams: %zd\n", i, req_ngrams.size(),
						le.request.c_str(),
						out.hashes.size());

				matched_req.emplace_back(out);
			}

		}

		void add_documents(int cpunum) {
			std::vector<std::thread> threads;

			for (int i = 0; i < cpunum; ++i) {
				struct doc_thread dth;

				dth.id = i;
				dth.step = cpunum;

				threads.emplace_back(std::bind(&learner::load_documents, this, dth));
			}

			for (int i = 0; i < cpunum; ++i) {
				threads[i].join();
			}
		}
};

int main(int argc, char *argv[])
{
	namespace bpo = boost::program_options;

	bpo::options_description generic("Similarity options");

	std::string mode, input, learn_file;
	generic.add_options()
		("help", "This help message")
		("input", bpo::value<std::string>(&input), "Input directory")
		("learn", bpo::value<std::string>(&learn_file), "Learning data file")
		("mode", bpo::value<std::string>(&mode)->default_value("learn"), "Processing mode: learn/check")
		;

	bpo::variables_map vm;

	try {
		bpo::store(bpo::parse_command_line(argc, argv, generic), vm);
		bpo::notify(vm);
	} catch (const std::exception &e) {
		std::cerr << "Invalid options: " << e.what() << "\n" << generic << std::endl;
		return -1;
	}

	if (!vm.count("input")) {
		std::cerr << "No input directory\n" << generic << std::endl;
		return -1;
	}

	if ((mode == "learn") && !vm.count("learn")) {
		std::cerr << "Learning mode requires file with learning data\n" << generic << std::endl;
		return -1;
	}

	xmlInitParser();

	if (mode == "learn") {
		learner l(input, learn_file);
		return -1;
	}
#if 0
	std::vector<document> docs;

	for (auto f : files) {
		try {
			p.feed(argv[i], 4);
			if (p.hashes().size() > 0)
				docs.emplace_back(argv[i], p.hashes());

#if 0
			std::cout << "================================" << std::endl;
			std::cout << argv[i] << ": hashes: " << p.hashes().size() << std::endl;
			std::ostringstream ss;
			std::vector<std::string> tokens = p.tokens();

			std::copy(tokens.begin(), tokens.end(), std::ostream_iterator<std::string>(ss, " "));
			std::cout << ss.str() << std::endl;
#endif
		} catch (const std::exception &e) {
			std::cerr << argv[i] << ": caught exception: " << e.what() << std::endl;
		}
	}


#endif
}
