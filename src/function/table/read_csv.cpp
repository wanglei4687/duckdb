#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/multi_file_reader.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/union_by_name.hpp"
#include "duckdb/execution/operator/persistent/csv_rejects_table.hpp"
#include "duckdb/execution/operator/scan/csv/csv_line_info.hpp"
#include "duckdb/execution/operator/scan/csv/csv_sniffer.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

#include <limits>

namespace duckdb {

unique_ptr<CSVFileHandle> ReadCSV::OpenCSV(const string &file_path, FileCompressionType compression,
                                           ClientContext &context) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto &allocator = BufferAllocator::Get(context);
	return CSVFileHandle::OpenFile(fs, allocator, file_path, compression);
}

void ReadCSVData::FinalizeRead(ClientContext &context) {
	BaseCSVData::Finalize();
	// We can't parallelize files with a mix of new lien delimiters, or with null padding options
	// Since we won't be able to detect where new lines start correctly
	bool not_supported_options = options.null_padding || options.dialect_options.new_line == NewLineIdentifier::MIX;

	auto number_of_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();
	// If we have many csv files, we run single-threaded on each file and parallelize on the number of files
	bool many_csv_files = files.size() > 1 && int64_t(files.size() * 2) >= number_of_threads;
	if (many_csv_files || not_supported_options) {
		// not supported for parallel CSV reading
		parallelize_single_file_scan = false;
	}
	// Validate rejects_table options
	if (!options.rejects_table_name.empty()) {
		if (!options.ignore_errors) {
			throw BinderException("REJECTS_TABLE option is only supported when IGNORE_ERRORS is set to true");
		}
		if (options.file_options.union_by_name) {
			throw BinderException("REJECTS_TABLE option is not supported when UNION_BY_NAME is set to true");
		}
	}

	if (!options.rejects_recovery_columns.empty()) {
		if (options.rejects_table_name.empty()) {
			throw BinderException(
			    "REJECTS_RECOVERY_COLUMNS option is only supported when REJECTS_TABLE is set to a table name");
		}
		for (auto &recovery_col : options.rejects_recovery_columns) {
			bool found = false;
			for (idx_t col_idx = 0; col_idx < return_names.size(); col_idx++) {
				if (StringUtil::CIEquals(return_names[col_idx], recovery_col)) {
					options.rejects_recovery_column_ids.push_back(col_idx);
					found = true;
					break;
				}
			}
			if (!found) {
				throw BinderException("Unsupported parameter for REJECTS_RECOVERY_COLUMNS: column \"%s\" not found",
				                      recovery_col);
			}
		}
	}

	if (options.rejects_limit != 0) {
		if (options.rejects_table_name.empty()) {
			throw BinderException("REJECTS_LIMIT option is only supported when REJECTS_TABLE is set to a table name");
		}
	}
}

static unique_ptr<FunctionData> ReadCSVBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {

	auto result = make_uniq<ReadCSVData>();
	auto &options = result->options;
	result->files = MultiFileReader::GetFileList(context, input.inputs[0], "CSV");

	options.FromNamedParameters(input.named_parameters, context, return_types, names);
	bool explicitly_set_columns = options.explicitly_set_columns;

	options.file_options.AutoDetectHivePartitioning(result->files, context);

	if (!options.auto_detect && return_types.empty()) {
		throw BinderException("read_csv requires columns to be specified through the 'columns' option. Use "
		                      "read_csv_auto or set read_csv(..., "
		                      "AUTO_DETECT=TRUE) to automatically guess columns.");
	}
	if (options.auto_detect) {
		options.file_path = result->files[0];
		result->buffer_manager = make_shared<CSVBufferManager>(context, options, result->files);
		CSVSniffer sniffer(options, result->buffer_manager, result->state_machine_cache, explicitly_set_columns);
		auto sniffer_result = sniffer.SniffCSV();
		if (names.empty()) {
			names = sniffer_result.names;
			return_types = sniffer_result.return_types;
		} else {
			if (explicitly_set_columns) {
				// The user has influenced the names, can't assume they are valid anymore
				if (return_types.size() != names.size()) {
					throw BinderException("The amount of names specified (%d) and the observed amount of types (%d) in "
					                      "the file don't match",
					                      names.size(), return_types.size());
				}
			} else {
				D_ASSERT(return_types.size() == names.size());
			}
		}

	} else {
		D_ASSERT(return_types.size() == names.size());
	}
	result->csv_types = return_types;
	result->csv_names = names;

	if (options.file_options.union_by_name) {
		D_ASSERT(0);
		//		result->reader_bind =
		//		    MultiFileReader::BindUnionReader<CSVScanner>(context, return_types, names, *result, options);
		//		if (result->union_readers.size() > 1) {
		//			result->column_info.emplace_back(result->csv_names, result->csv_types);
		//			for (idx_t i = 1; i < result->union_readers.size(); i++) {
		//				result->column_info.emplace_back(result->union_readers[i]->names,
		//				                                 result->union_readers[i]->return_types);
		//			}
		//		}
		//		if (!options.sql_types_per_column.empty()) {
		//			auto exception = CSVScanner::ColumnTypesError(options.sql_types_per_column, names);
		//			if (!exception.empty()) {
		//				throw BinderException(exception);
		//			}
		//		}
	} else {
		result->reader_bind = MultiFileReader::BindOptions(options.file_options, result->files, return_types, names);
	}
	result->return_types = return_types;
	result->return_names = names;
	result->FinalizeRead(context);

	return std::move(result);
}

static unique_ptr<FunctionData> ReadCSVAutoBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	input.named_parameters["auto_detect"] = Value::BOOLEAN(true);
	return ReadCSVBind(context, input, return_types, names);
}

//===--------------------------------------------------------------------===//
// Parallel CSV Reader CSV Global State
//===--------------------------------------------------------------------===//

struct CSVGlobalState : public GlobalTableFunctionState {
public:
	CSVGlobalState(ClientContext &context, shared_ptr<CSVBufferManager> buffer_manager_p,
	                       const CSVReaderOptions &options, idx_t system_threads_p, const vector<string> &files,
	                       bool force_parallelism_p, vector<column_t> column_ids_p)
	    : buffer_manager(std::move(buffer_manager_p)), system_threads(system_threads_p),
	      force_parallelism(force_parallelism_p), column_ids(std::move(column_ids_p)),
	      line_info(main_mutex, batch_to_tuple_end, tuple_start, tuple_end) {

		// If the buffer manager has not yet being initialized, we do it now.
		if (!buffer_manager) {
			buffer_manager = make_shared<CSVBufferManager>(context, options, files);
		}
		//! Set information regarding file_size, if it's on disk and use that to set number of threads that will
		//! be used in this scanner
		file_size = buffer_manager->file_handle->FileSize();
		on_disk_file = buffer_manager->file_handle->OnDiskFile();
		running_threads = MaxThreads();

		// Initialize all the book-keeping variables
		InitializeVerificationVariables(files.size());

		// Initialize the lines read
		line_info.lines_read[0][0] = options.dialect_options.skip_rows;
		if (options.has_header && options.dialect_options.header) {
			line_info.lines_read[0][0]++;
		}
		first_position = options.dialect_options.true_start;
	}
	explicit CSVGlobalState(idx_t system_threads_p)
	    : system_threads(system_threads_p), line_info(main_mutex, batch_to_tuple_end, tuple_start, tuple_end) {
		running_threads = MaxThreads();
	}

	~CSVGlobalState() override {
	}

	void InitializeVerificationVariables(idx_t file_count){
		line_info.current_batches.resize(file_count);
		line_info.lines_read.resize(file_count);
		line_info.lines_errored.resize(file_count);
		tuple_start.resize(file_count);
		tuple_end.resize(file_count);
		tuple_end_to_batch.resize(file_count);
		batch_to_tuple_end.resize(file_count);

	}
	//! How many bytes were read up to this point
	atomic<idx_t> bytes_read {0};
	//! Size of the first file
	// TODO: we now have an assumption that all files have similar sizes, we possibly should do more fine-grained
	// Optimizations to multi-file readers.
	idx_t file_size;

public:
	idx_t MaxThreads() const override;

	//! Generates a CSV Scanner, with information regarding the piece of buffer it should be read.
	//! In case it returns a nullptr it means we are done reading these files.
	unique_ptr<CSVScanner> Next(ClientContext &context, const ReadCSVData &bind_data);
	//! Verify if the CSV File was read correctly
	void Verify();

	void UpdateVerification(VerificationPositions positions, idx_t file_number, idx_t batch_idx);

	void UpdateLinesRead(CSVScanner &buffer_read, idx_t file_idx);

	void DecrementThread();

	bool Finished();

	double GetProgress(const ReadCSVData &bind_data) const {
		idx_t total_files = bind_data.files.size();
		// get the progress WITHIN the current file
		double progress;
		if (file_size == 0) {
			progress = 1.0;
		} else {
			progress = double(bytes_read) / double(file_size);
		}
		// now get the total percentage of files read
		double percentage = double(cur_file_idx) / total_files;
		percentage += (double(1) / double(total_files)) * progress;
		return percentage * 100;
	}

private:
	//! File Handle for current file
	shared_ptr<CSVBufferManager> buffer_manager;
	//! Information to set CSV Scanners
	//! The index of the current file being read
	idx_t cur_file_idx = 0;
	//! The index of the current buffer from the current file
	idx_t cur_buffer_idx = 0;
	//! How many bytes each thread should read
	const idx_t bytes_per_thread = 0;

	//! Mutex to lock when getting next batch of bytes (Parallel Only)
	mutex main_mutex;
	//! Byte set from for last thread
	idx_t next_byte = 0;
	//! Whether or not this is an on-disk file
	bool on_disk_file = true;
	//! Basically max number of threads in DuckDB
	idx_t system_threads;
	//! Current batch index
	idx_t batch_index = 0;
	idx_t local_batch_index = 0;

	//! Forces parallelism for small CSV Files, should only be used for testing.
	bool force_parallelism = false;
	//! First Position of First Buffer
	idx_t first_position = 0;
	//! Current File Number
	idx_t max_tuple_end = 0;

	//! The vector stores positions where threads ended the last line they read in the CSV File, and the set stores
	//! Positions where they started reading the first line.
	vector<vector<idx_t>> tuple_end;
	vector<set<idx_t>> tuple_start;
	//! Tuple end to batch
	vector<unordered_map<idx_t, idx_t>> tuple_end_to_batch;
	//! Batch to Tuple End
	vector<unordered_map<idx_t, idx_t>> batch_to_tuple_end;
	idx_t running_threads = 0;
	//! The column ids to read
	vector<column_t> column_ids;
	//! Line Info used in error messages
	LineInfo line_info;
};

idx_t CSVGlobalState::MaxThreads() const {
	if (force_parallelism || !on_disk_file) {
		// If parallelism is forced we run with max threads, this should only be used for tested or by
		// advanced users
		// If file is not on disk, in many cases we can't guess how big it is, hence we also run with max threads.
		return system_threads;
	}
	// We initialize max one thread per Mb
	const idx_t one_mb = 1000000;
	idx_t threads_per_mb = file_size / one_mb + 1;
	if (threads_per_mb < system_threads || threads_per_mb == 1) {
		return threads_per_mb;
	}

	return system_threads;
}

void CSVGlobalState::DecrementThread() {
	lock_guard<mutex> parallel_lock(main_mutex);
	D_ASSERT(running_threads > 0);
	running_threads--;
}

bool CSVGlobalState::Finished() {
	lock_guard<mutex> parallel_lock(main_mutex);
	return running_threads == 0;
}

void CSVGlobalState::Verify() {
	// All threads are done, we run some magic sweet verification code
	lock_guard<mutex> parallel_lock(main_mutex);
	if (running_threads == 0) {
		D_ASSERT(tuple_end.size() == tuple_start.size());
		for (idx_t i = 0; i < tuple_start.size(); i++) {
			auto &current_tuple_end = tuple_end[i];
			auto &current_tuple_start = tuple_start[i];
			// figure out max value of last_pos
			if (current_tuple_end.empty()) {
				return;
			}
			auto max_value = *max_element(std::begin(current_tuple_end), std::end(current_tuple_end));
			for (idx_t tpl_idx = 0; tpl_idx < current_tuple_end.size(); tpl_idx++) {
				auto last_pos = current_tuple_end[tpl_idx];
				auto first_pos = current_tuple_start.find(last_pos);
				if (first_pos == current_tuple_start.end()) {
					// this might be necessary due to carriage returns outside buffer scopes.
					first_pos = current_tuple_start.find(last_pos + 1);
				}
				if (first_pos == current_tuple_start.end() && last_pos != max_value) {
					auto batch_idx = tuple_end_to_batch[i][last_pos];
					auto problematic_line = line_info.GetLine(batch_idx);
					throw InvalidInputException(
					    "CSV File not supported for multithreading. This can be a problematic line in your CSV File or "
					    "that this CSV can't be read in Parallel. Please, inspect if the line %llu is correct. If so, "
					    "please run single-threaded CSV Reading by setting parallel=false in the read_csv call.",
					    problematic_line);
				}
			}
		}
	}
}

void LineInfo::Verify(idx_t file_idx, idx_t batch_idx, idx_t cur_first_pos) {
	auto &tuple_start_set = tuple_start[file_idx];
	auto &processed_batches = batch_to_tuple_end[file_idx];
	auto &tuple_end_vec = tuple_end[file_idx];
	bool has_error = false;
	idx_t problematic_line;
	if (batch_idx == 0 || tuple_start_set.empty()) {
		return;
	}
	for (idx_t cur_batch = 0; cur_batch < batch_idx - 1; cur_batch++) {
		auto cur_end = tuple_end_vec[processed_batches[cur_batch]];
		auto first_pos = tuple_start_set.find(cur_end);
		if (first_pos == tuple_start_set.end()) {
			has_error = true;
			problematic_line = GetLine(cur_batch);
			break;
		}
	}
	if (!has_error) {
		auto cur_end = tuple_end_vec[processed_batches[batch_idx - 1]];
		if (cur_end != cur_first_pos) {
			has_error = true;
			problematic_line = GetLine(batch_idx);
		}
	}
	if (has_error) {
		throw InvalidInputException(
		    "CSV File not supported for multithreading. This can be a problematic line in your CSV File or "
		    "that this CSV can't be read in Parallel. Please, inspect if the line %llu is correct. If so, "
		    "please run single-threaded CSV Reading by setting parallel=false in the read_csv call.",
		    problematic_line);
	}
}
unique_ptr<CSVScanner> CSVGlobalState::Next(ClientContext &context, const ReadCSVData &bind_data) {
	lock_guard<mutex> parallel_lock(main_mutex);

	// Generate CSV Iterator
	CSVIterator csv_iterator (cur_file_idx, cur_buffer_idx, cur_, bytes_to_read );

	// set up the current buffer
	line_info.current_batches[file_index - 1].insert(local_batch_index);
	idx_t bytes_per_local_state = current_buffer->actual_size / MaxThreads() + 1;
	auto result =
	    make_uniq<CSVBuffer>(buffer_manager->GetBuffer(file_index - 1, cur_buffer_idx),
	                             buffer_manager->GetBuffer(file_index - 1, cur_buffer_idx + 1), next_byte,
	                             next_byte + bytes_per_local_state, batch_index++, local_batch_index++, &line_info);
	// move the byte index of the CSV reader to the next buffer
	next_byte += bytes_per_local_state;
	if (next_byte >= current_buffer->actual_size) {
		// We replace the current buffer with the next buffer
		next_byte = 0;
		bytes_read += current_buffer->actual_size;
		current_buffer = std::move(next_buffer);
		cur_buffer_idx++;
		if (current_buffer) {
			// Next buffer gets the next-next buffer
			next_buffer = buffer_manager->GetBuffer(file_index - 1, cur_buffer_idx + 1);
		}
	}
	if (!reader) {
		D_ASSERT(0);
		//		// we either don't have a reader, or the reader was created for a different file
		//		// we need to create a new reader and instantiate it
		//		if (file_index > 0 && file_index <= bind_data.union_readers.size() && bind_data.union_readers[file_index
		//- 1]) {
		//			// we are doing UNION BY NAME - fetch the options from the union reader for this file
		//			auto &union_reader = *bind_data.union_readers[file_index - 1];
		//			reader = make_uniq<ParallelCSVReader>(context, union_reader.options, std::move(result),
		// first_position, 			                                      union_reader.GetTypes(), file_index - 1);
		// reader->names = union_reader.GetNames(); 		} else if (file_index <= bind_data.column_info.size()) {
		//			// Serialized Union By name
		//			reader = make_uniq<ParallelCSVReader>(context, bind_data.options, std::move(result), first_position,
		//			                                      bind_data.column_info[file_index - 1].types, file_index - 1);
		//			reader->names = bind_data.column_info[file_index - 1].names;
		//		} else {
		//			// regular file - use the standard options
		//			if (!result) {
		//				return false;
		//			}
		//			reader = make_uniq<ParallelCSVReader>(context, bind_data.options, std::move(result), first_position,
		//			                                      bind_data.csv_types, file_index - 1);
		//			reader->names = bind_data.csv_names;
		//		}
		//		reader->options.file_path = current_file_path;
		//		MultiFileReader::InitializeReader(*reader, bind_data.options.file_options, bind_data.reader_bind,
		//		                                  bind_data.return_types, bind_data.return_names, column_ids, nullptr,
		//		                                  bind_data.files.front(), context);
	} else {
		// update the current reader
//		reader->SetBufferRead(std::move(result));
	}

	return true;
}
void CSVGlobalState::UpdateVerification(VerificationPositions positions, idx_t file_number_p, idx_t batch_idx) {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (positions.end_of_last_line > max_tuple_end) {
		max_tuple_end = positions.end_of_last_line;
	}
	tuple_end_to_batch[file_number_p][positions.end_of_last_line] = batch_idx;
	batch_to_tuple_end[file_number_p][batch_idx] = tuple_end[file_number_p].size();
	tuple_start[file_number_p].insert(positions.beginning_of_first_line);
	tuple_end[file_number_p].push_back(positions.end_of_last_line);
}

void CSVGlobalState::UpdateLinesRead(CSVScanner &buffer_read, idx_t file_idx) {
	auto batch_idx = buffer_read.scanner_id;
	auto lines_read = buffer_read.GetTotalRowsEmmited();
	lock_guard<mutex> parallel_lock(main_mutex);
	line_info.current_batches[file_idx].erase(batch_idx);
	line_info.lines_read[file_idx][batch_idx] += lines_read;
}

bool LineInfo::CanItGetLine(idx_t file_idx, idx_t batch_idx) {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (current_batches.empty() || done) {
		return true;
	}
	if (file_idx >= current_batches.size() || current_batches[file_idx].empty()) {
		return true;
	}
	auto min_value = *current_batches[file_idx].begin();
	if (min_value >= batch_idx) {
		return true;
	}
	return false;
}

void LineInfo::Increment(idx_t file_idx, idx_t batch_idx) {
	auto parallel_lock = duckdb::make_uniq<lock_guard<mutex>>(main_mutex);
	lines_errored[file_idx][batch_idx]++;
}

// Returns the 1-indexed line number
idx_t LineInfo::GetLine(idx_t batch_idx, idx_t line_error, idx_t file_idx, idx_t cur_start, bool verify,
                        bool stop_at_first) {
	unique_ptr<lock_guard<mutex>> parallel_lock;
	if (!verify) {
		parallel_lock = duckdb::make_uniq<lock_guard<mutex>>(main_mutex);
	}
	idx_t line_count = 0;

	if (!stop_at_first) {
		// Figure out the amount of lines read in the current file
		for (idx_t cur_batch_idx = 0; cur_batch_idx <= batch_idx; cur_batch_idx++) {
			if (cur_batch_idx < batch_idx) {
				line_count += lines_errored[file_idx][cur_batch_idx];
			}
			line_count += lines_read[file_idx][cur_batch_idx];
		}
		return line_count + line_error + 1;
	}

	// Otherwise, check if we already have an error on another thread
	if (done) {
		// line count is 0-indexed, but we want to return 1-indexed
		return first_line + 1;
	}
	for (idx_t i = 0; i <= batch_idx; i++) {
		if (lines_read[file_idx].find(i) == lines_read[file_idx].end() && i != batch_idx) {
			throw InternalException("Missing batch index on Parallel CSV Reader GetLine");
		}
		line_count += lines_read[file_idx][i];
	}

	// before we are done, if this is not a call in Verify() we must check Verify up to this batch
	if (!verify) {
		Verify(file_idx, batch_idx, cur_start);
	}
	done = true;
	first_line = line_count + line_error;
	// line count is 0-indexed, but we want to return 1-indexed
	return first_line + 1;
}

//===--------------------------------------------------------------------===//
// Read CSV Local State
//===--------------------------------------------------------------------===//
struct CSVLocalState : public LocalTableFunctionState {
public:
	explicit CSVLocalState(unique_ptr<CSVScanner> csv_reader_p) : csv_reader(std::move(csv_reader_p)) {
	}

	//! The CSV reader
	unique_ptr<CSVScanner> csv_reader;
	bool done = false;
};

//===--------------------------------------------------------------------===//
// Read CSV Functions
//===--------------------------------------------------------------------===//
static unique_ptr<GlobalTableFunctionState> ReadCSVInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ReadCSVData>();

	// Create the temporary rejects table
	auto rejects_table = bind_data.options.rejects_table_name;
	if (!rejects_table.empty()) {
		CSVRejectsTable::GetOrCreate(context, rejects_table)->InitializeTable(context, bind_data);
	}
	if (bind_data.files.empty()) {
		// This can happen when a filename based filter pushdown has eliminated all possible files for this scan.
		return make_uniq<CSVGlobalState>(context.db->NumberOfThreads());
	}
	return make_uniq<CSVGlobalState>(context, bind_data.buffer_manager, bind_data.options, context.db->NumberOfThreads(),
	                                         bind_data.files, ClientConfig::GetConfig(context).verify_parallelism,
	                                         input.column_ids);
}

unique_ptr<LocalTableFunctionState> ReadCSVInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                     GlobalTableFunctionState *global_state_p) {
	auto &csv_data = input.bind_data->Cast<ReadCSVData>();
	auto &global_state = global_state_p->Cast<CSVGlobalState>();
	unique_ptr<CSVScanner> csv_scanner;
	auto has_next = global_state.Next(context.client, csv_data, csv_scanner);
	if (!has_next) {
		global_state.DecrementThread();
		csv_scanner.reset();
	}
	return make_uniq<CSVLocalState>(std::move(csv_scanner));
}

static void ReadCSVFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<ReadCSVData>();
	auto &csv_global_state = data_p.global_state->Cast<CSVGlobalState>();
	auto &csv_local_state = data_p.local_state->Cast<CSVLocalState>();

	if (!csv_local_state.csv_reader) {
		// no csv_reader was set, this can happen when a filename-based filter has filtered out all possible files
		return;
	}
	do {
		if (output.size() != 0) {
			MultiFileReader::FinalizeChunk(bind_data.reader_bind, csv_local_state.csv_reader->reader_data, output);
			break;
		}
		if (csv_local_state.csv_reader->finished) {
			auto verification_updates = csv_local_state.csv_reader->GetVerificationPositions();
			csv_global_state.UpdateVerification(verification_updates, csv_local_state.csv_reader->file_idx,
			                                    csv_local_state.csv_reader->scanner->scanner_id);
			csv_global_state.UpdateLinesRead(*csv_local_state.csv_reader->scanner,
			                                 csv_local_state.csv_reader->file_idx);
			auto has_next = csv_global_state.Next(context, bind_data, csv_local_state.csv_reader);
			if (csv_local_state.csv_reader) {
				csv_local_state.csv_reader->linenr = 0;
			}
			if (!has_next) {
				csv_global_state.DecrementThread();
				break;
			}
		}
		csv_local_state.csv_reader->Parse(output);

	} while (true);
	if (csv_global_state.Finished()) {
		csv_global_state.Verify();
	}
}

static idx_t CSVReaderGetBatchIndex(ClientContext &context, const FunctionData *bind_data_p,
                                    LocalTableFunctionState *local_state, GlobalTableFunctionState *global_state) {
	auto &data = local_state->Cast<CSVLocalState>();
	return data.csv_reader->scanner_id;
}

static void ReadCSVAddNamedParameters(TableFunction &table_function) {
	table_function.named_parameters["sep"] = LogicalType::VARCHAR;
	table_function.named_parameters["delim"] = LogicalType::VARCHAR;
	table_function.named_parameters["quote"] = LogicalType::VARCHAR;
	table_function.named_parameters["new_line"] = LogicalType::VARCHAR;
	table_function.named_parameters["escape"] = LogicalType::VARCHAR;
	table_function.named_parameters["nullstr"] = LogicalType::VARCHAR;
	table_function.named_parameters["columns"] = LogicalType::ANY;
	table_function.named_parameters["auto_type_candidates"] = LogicalType::ANY;
	table_function.named_parameters["header"] = LogicalType::BOOLEAN;
	table_function.named_parameters["auto_detect"] = LogicalType::BOOLEAN;
	table_function.named_parameters["sample_size"] = LogicalType::BIGINT;
	table_function.named_parameters["all_varchar"] = LogicalType::BOOLEAN;
	table_function.named_parameters["dateformat"] = LogicalType::VARCHAR;
	table_function.named_parameters["timestampformat"] = LogicalType::VARCHAR;
	table_function.named_parameters["normalize_names"] = LogicalType::BOOLEAN;
	table_function.named_parameters["compression"] = LogicalType::VARCHAR;
	table_function.named_parameters["skip"] = LogicalType::BIGINT;
	table_function.named_parameters["max_line_size"] = LogicalType::VARCHAR;
	table_function.named_parameters["maximum_line_size"] = LogicalType::VARCHAR;
	table_function.named_parameters["ignore_errors"] = LogicalType::BOOLEAN;
	table_function.named_parameters["rejects_table"] = LogicalType::VARCHAR;
	table_function.named_parameters["rejects_limit"] = LogicalType::BIGINT;
	table_function.named_parameters["rejects_recovery_columns"] = LogicalType::LIST(LogicalType::VARCHAR);
	table_function.named_parameters["buffer_size"] = LogicalType::UBIGINT;
	table_function.named_parameters["decimal_separator"] = LogicalType::VARCHAR;
	table_function.named_parameters["parallel"] = LogicalType::BOOLEAN;
	table_function.named_parameters["null_padding"] = LogicalType::BOOLEAN;
	table_function.named_parameters["allow_quoted_nulls"] = LogicalType::BOOLEAN;
	table_function.named_parameters["column_types"] = LogicalType::ANY;
	table_function.named_parameters["dtypes"] = LogicalType::ANY;
	table_function.named_parameters["types"] = LogicalType::ANY;
	table_function.named_parameters["names"] = LogicalType::LIST(LogicalType::VARCHAR);
	table_function.named_parameters["column_names"] = LogicalType::LIST(LogicalType::VARCHAR);
	MultiFileReader::AddParameters(table_function);
}

double CSVReaderProgress(ClientContext &context, const FunctionData *bind_data_p,
                         const GlobalTableFunctionState *global_state) {
	auto &bind_data = bind_data_p->Cast<ReadCSVData>();
	auto &data = global_state->Cast<CSVGlobalState>();
	return data.GetProgress(bind_data);
}

void CSVComplexFilterPushdown(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                              vector<unique_ptr<Expression>> &filters) {
	auto &data = bind_data_p->Cast<ReadCSVData>();
	auto reset_reader =
	    MultiFileReader::ComplexFilterPushdown(context, data.files, data.options.file_options, get, filters);
	if (reset_reader) {
		D_ASSERT(0);
		//		MultiFileReader::PruneReaders(data);
	}
}

unique_ptr<NodeStatistics> CSVReaderCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<ReadCSVData>();
	idx_t per_file_cardinality = 0;
	if (bind_data.buffer_manager && bind_data.buffer_manager->file_handle) {
		auto estimated_row_width = (bind_data.csv_types.size() * 5);
		per_file_cardinality = bind_data.buffer_manager->file_handle->FileSize() / estimated_row_width;
	} else {
		// determined through the scientific method as the average amount of rows in a CSV file
		per_file_cardinality = 42;
	}
	return make_uniq<NodeStatistics>(bind_data.files.size() * per_file_cardinality);
}

static void CSVReaderSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                               const TableFunction &function) {
	auto &bind_data = bind_data_p->Cast<ReadCSVData>();
	serializer.WriteProperty(100, "extra_info", function.extra_info);
	serializer.WriteProperty(101, "csv_data", &bind_data);
}

static unique_ptr<FunctionData> CSVReaderDeserialize(Deserializer &deserializer, TableFunction &function) {
	unique_ptr<ReadCSVData> result;
	deserializer.ReadProperty(100, "extra_info", function.extra_info);
	deserializer.ReadProperty(101, "csv_data", result);
	return std::move(result);
}

TableFunction ReadCSVTableFunction::GetFunction() {
	TableFunction read_csv("read_csv", {LogicalType::VARCHAR}, ReadCSVFunction, ReadCSVBind, ReadCSVInitGlobal,
	                       ReadCSVInitLocal);
	read_csv.table_scan_progress = CSVReaderProgress;
	read_csv.pushdown_complex_filter = CSVComplexFilterPushdown;
	read_csv.serialize = CSVReaderSerialize;
	read_csv.deserialize = CSVReaderDeserialize;
	read_csv.get_batch_index = CSVReaderGetBatchIndex;
	read_csv.cardinality = CSVReaderCardinality;
	read_csv.projection_pushdown = true;
	ReadCSVAddNamedParameters(read_csv);
	return read_csv;
}

TableFunction ReadCSVTableFunction::GetAutoFunction() {
	auto read_csv_auto = ReadCSVTableFunction::GetFunction();
	read_csv_auto.name = "read_csv_auto";
	read_csv_auto.bind = ReadCSVAutoBind;
	return read_csv_auto;
}

void ReadCSVTableFunction::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(MultiFileReader::CreateFunctionSet(ReadCSVTableFunction::GetFunction()));
	set.AddFunction(MultiFileReader::CreateFunctionSet(ReadCSVTableFunction::GetAutoFunction()));
}

unique_ptr<TableRef> ReadCSVReplacement(ClientContext &context, const string &table_name, ReplacementScanData *data) {
	auto lower_name = StringUtil::Lower(table_name);
	// remove any compression
	if (StringUtil::EndsWith(lower_name, ".gz")) {
		lower_name = lower_name.substr(0, lower_name.size() - 3);
	} else if (StringUtil::EndsWith(lower_name, ".zst")) {
		if (!Catalog::TryAutoLoad(context, "parquet")) {
			throw MissingExtensionException("parquet extension is required for reading zst compressed file");
		}
		lower_name = lower_name.substr(0, lower_name.size() - 4);
	}
	if (!StringUtil::EndsWith(lower_name, ".csv") && !StringUtil::Contains(lower_name, ".csv?") &&
	    !StringUtil::EndsWith(lower_name, ".tsv") && !StringUtil::Contains(lower_name, ".tsv?")) {
		return nullptr;
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
	table_function->function = make_uniq<FunctionExpression>("read_csv_auto", std::move(children));

	if (!FileSystem::HasGlob(table_name)) {
		auto &fs = FileSystem::GetFileSystem(context);
		table_function->alias = fs.ExtractBaseName(table_name);
	}

	return std::move(table_function);
}

void BuiltinFunctions::RegisterReadFunctions() {
	CSVCopyFunction::RegisterFunction(*this);
	ReadCSVTableFunction::RegisterFunction(*this);
	auto &config = DBConfig::GetConfig(*transaction.db);
	config.replacement_scans.emplace_back(ReadCSVReplacement);
}

} // namespace duckdb
