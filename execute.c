/*-------
 * Module:			execute.c
 *
 * Description:		This module contains routines related to
 *					preparing and executing an SQL statement.
 *
 * Classes:			n/a
 *
 * API functions:	SQLPrepare, SQLExecute, SQLExecDirect, SQLTransact,
 *					SQLCancel, SQLNativeSql, SQLParamData, SQLPutData
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *-------
 */

#include "psqlodbc.h"

#include <stdio.h>
#include <string.h>

#include "environ.h"
#include "connection.h"
#include "statement.h"
#include "qresult.h"
#include "convert.h"
#include "bind.h"
#include "pgtypes.h"
#include "lobj.h"
#include "pgapifunc.h"

/*extern GLOBAL_VALUES globals;*/


/*		Perform a Prepare on the SQL statement */
RETCODE		SQL_API
PGAPI_Prepare(HSTMT hstmt,
			  UCHAR FAR * szSqlStr,
			  SDWORD cbSqlStr)
{
	CSTR func = "PGAPI_Prepare";
	StatementClass *self = (StatementClass *) hstmt;

	mylog("%s: entering...\n", func);

	if (!self)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	/*
	 * According to the ODBC specs it is valid to call SQLPrepare multiple
	 * times. In that case, the bound SQL statement is replaced by the new
	 * one
	 */

	switch (self->status)
	{
		case STMT_PREMATURE:
			mylog("**** PGAPI_Prepare: STMT_PREMATURE, recycle\n");
			SC_recycle_statement(self); /* recycle the statement, but do
										 * not remove parameter bindings */
			break;

		case STMT_FINISHED:
			mylog("**** PGAPI_Prepare: STMT_FINISHED, recycle\n");
			SC_recycle_statement(self); /* recycle the statement, but do
										 * not remove parameter bindings */
			break;

		case STMT_ALLOCATED:
			mylog("**** PGAPI_Prepare: STMT_ALLOCATED, copy\n");
			self->status = STMT_READY;
			break;

		case STMT_READY:
			mylog("**** PGAPI_Prepare: STMT_READY, change SQL\n");
			break;

		case STMT_EXECUTING:
			mylog("**** PGAPI_Prepare: STMT_EXECUTING, error!\n");

			SC_set_error(self, STMT_SEQUENCE_ERROR, "PGAPI_Prepare(): The handle does not point to a statement that is ready to be executed");
			SC_log_error(func, "", self);

			return SQL_ERROR;

		default:
			SC_set_error(self, STMT_INTERNAL_ERROR, "An Internal Error has occured -- Unknown statement status.");
			SC_log_error(func, "", self);
			return SQL_ERROR;
	}

	SC_initialize_stmts(self, TRUE);

	if (!szSqlStr)
	{
		SC_set_error(self, STMT_NO_MEMORY_ERROR, "the query is NULL");
		SC_log_error(func, "", self);
		return SQL_ERROR;
	}
	if (!szSqlStr[0])
		self->statement = strdup("");
	else
		self->statement = make_string(szSqlStr, cbSqlStr, NULL, 0);
	if (!self->statement)
	{
		SC_set_error(self, STMT_NO_MEMORY_ERROR, "No memory available to store statement");
		SC_log_error(func, "", self);
		return SQL_ERROR;
	}

	self->prepare = TRUE;
	SC_set_prepared(self, FALSE);
	self->statement_type = statement_type(self->statement);

	/* Check if connection is onlyread (only selects are allowed) */
	if (CC_is_onlyread(self->hdbc) && STMT_UPDATE(self))
	{
		SC_set_error(self, STMT_EXEC_ERROR, "Connection is readonly, only select statements are allowed.");
		SC_log_error(func, "", self);
		return SQL_ERROR;
	}

	return SQL_SUCCESS;
}


/*		Performs the equivalent of SQLPrepare, followed by SQLExecute. */
RETCODE		SQL_API
PGAPI_ExecDirect(
				 HSTMT hstmt,
				 UCHAR FAR * szSqlStr,
				 SDWORD cbSqlStr,
				 UWORD flag)
{
	StatementClass *stmt = (StatementClass *) hstmt;
	RETCODE		result;
	CSTR func = "PGAPI_ExecDirect";

	mylog("%s: entering...\n", func);

	if (result = SC_initialize_and_recycle(stmt), SQL_SUCCESS != result)
		return result;

	/*
	 * keep a copy of the un-parametrized statement, in case they try to
	 * execute this statement again
	 */
	stmt->statement = make_string(szSqlStr, cbSqlStr, NULL, 0);
	if (!stmt->statement)
	{
		SC_set_error(stmt, STMT_NO_MEMORY_ERROR, "No memory available to store statement");
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	mylog("**** %s: hstmt=%u, statement='%s'\n", func, hstmt, stmt->statement);

	/*
	 * If an SQLPrepare was performed prior to this, but was left in the
	 * premature state because an error occurred prior to SQLExecute then
	 * set the statement to finished so it can be recycled.
	 */
	if (stmt->status == STMT_PREMATURE)
		stmt->status = STMT_FINISHED;

	stmt->statement_type = statement_type(stmt->statement);

	/* Check if connection is onlyread (only selects are allowed) */
	if (CC_is_onlyread(stmt->hdbc) && STMT_UPDATE(stmt))
	{
		SC_set_error(stmt, STMT_EXEC_ERROR, "Connection is readonly, only select statements are allowed.");
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	mylog("%s: calling PGAPI_Execute...\n", func);

	result = PGAPI_Execute(hstmt, flag);

	mylog("%s: returned %hd from PGAPI_Execute\n", func, result);
	return result;
}

/*
 *	The execution after all parameters were resolved.
 */
static
RETCODE	Exec_with_parameters_resolved(StatementClass *stmt, BOOL *exec_end)
{
	CSTR func = "Exec_with_parameters_resolved";
	RETCODE		retval;
	int		end_row, cursor_type, scroll_concurrency;
	ConnectionClass	*conn;
	QResultClass	*res;
	APDFields	*apdopts;
	IPDFields	*ipdopts;
	BOOL		prepare_before_exec = FALSE;

	*exec_end = FALSE;
	conn = SC_get_conn(stmt);
	mylog("%s: copying statement params: trans_status=%d, len=%d, stmt='%s'\n", func, conn->transact_status, strlen(stmt->statement), stmt->statement);

	/* save the cursor's info before the execution */
	cursor_type = stmt->options.cursor_type;
	scroll_concurrency = stmt->options.scroll_concurrency;
	/* Prepare the statement if possible at backend side */
	if (stmt->prepare &&
	    !stmt->prepared &&
	    !stmt->inaccurate_result &&
	    conn->connInfo.use_server_side_prepare &&
	    PG_VERSION_GE(conn, 7.3))
		prepare_before_exec = TRUE;
	/* Create the statement with parameters substituted. */
	retval = copy_statement_with_parameters(stmt, prepare_before_exec);
	stmt->current_exec_param = -1;
	if (retval != SQL_SUCCESS)
	{
		stmt->exec_current_row = -1;
		*exec_end = TRUE;
		return retval; /* error msg is passed from the above */
	}

	mylog("   stmt_with_params = '%s'\n", stmt->stmt_with_params);

	/*
	 *	Dummy exection to get the column info.
	 */ 
	if (stmt->inaccurate_result && conn->connInfo.disallow_premature)
	{
		BOOL		in_trans = CC_is_in_trans(conn);
		BOOL		issued_begin = FALSE,
					begin_included = FALSE;
		QResultClass *curres;

		stmt->exec_current_row = -1;
		*exec_end = TRUE;
		if (!SC_is_pre_executable(stmt))
			return SQL_SUCCESS;
		if (strnicmp(stmt->stmt_with_params, "BEGIN;", 6) == 0)
			begin_included = TRUE;
		else if (!in_trans)
		{
			if (issued_begin = CC_begin(conn), !issued_begin)
			{
				SC_set_error(stmt, STMT_EXEC_ERROR,  "Handle prepare error");
				return SQL_ERROR;
			}
		}
		/* we are now in a transaction */
		res = CC_send_query(conn, stmt->stmt_with_params, NULL, CLEAR_RESULT_ON_ABORT);
		if (!res)
		{
			CC_abort(conn);
			SC_set_error(stmt, STMT_EXEC_ERROR, "Handle prepare error");
			return SQL_ERROR;
		}
		SC_set_Result(stmt, res);
		for (curres = res; !curres->num_fields; curres = curres->next)
			;
		SC_set_Curres(stmt, curres);
		if (CC_is_in_autocommit(conn))
		{
			if (issued_begin)
				CC_commit(conn);
		}
		stmt->status = STMT_FINISHED;
		return SQL_SUCCESS;
	}
	/*
	 *	The real execution.
	 */
	retval = SC_execute(stmt);
	if (retval == SQL_ERROR)
	{
		stmt->exec_current_row = -1;
		*exec_end = TRUE;
		return retval;
	}
	res = SC_get_Result(stmt);
	/* special handling of result for keyset driven cursors */
	if (SQL_CURSOR_KEYSET_DRIVEN == stmt->options.cursor_type &&
	    SQL_CONCUR_READ_ONLY != stmt->options.scroll_concurrency)
	{
		QResultClass	*kres;

		if (kres = res->next, kres)
		{
			kres->fields = res->fields;
			res->fields = NULL;
			kres->num_fields = res->num_fields;
			res->next = NULL;
			QR_Destructor(res);
			SC_set_Result(stmt, kres);
			res = kres;
		}
	}
	else if (SC_is_prepare_before_exec(stmt))
	{
		if (res && QR_command_maybe_successful(res))
		{
			QResultClass	*kres;
		
			kres = res->next;
			SC_set_Result(stmt, kres);
			res->next = NULL;
			QR_Destructor(res);
			res = kres;
			SC_set_prepared(stmt, TRUE);
		}
		else
		{
			retval = SQL_ERROR;
			if (stmt->execute_statement)
				free(stmt->execute_statement);
			stmt->execute_statement = NULL;
		}
	}
#if (ODBCVER >= 0x0300)
	ipdopts = SC_get_IPDF(stmt);
	if (ipdopts->param_status_ptr)
	{
		switch (retval)
		{
			case SQL_SUCCESS: 
				ipdopts->param_status_ptr[stmt->exec_current_row] = SQL_PARAM_SUCCESS;
				break;
			case SQL_SUCCESS_WITH_INFO: 
				ipdopts->param_status_ptr[stmt->exec_current_row] = SQL_PARAM_SUCCESS_WITH_INFO;
				break;
			default: 
				ipdopts->param_status_ptr[stmt->exec_current_row] = SQL_PARAM_ERROR;
				break;
		}
	}
#endif /* ODBCVER */
	if (end_row = stmt->exec_end_row, end_row < 0)
	{
		apdopts = SC_get_APDF(stmt);
		end_row = apdopts->paramset_size - 1;
	}
	if (stmt->inaccurate_result ||
	    stmt->exec_current_row >= end_row)
	{
		*exec_end = TRUE;
		stmt->exec_current_row = -1;
	}
	else
		stmt->exec_current_row++;
	if (res)
	{
#if (ODBCVER >= 0x0300)
        	EnvironmentClass *env = (EnvironmentClass *) (conn->henv);
		const char *cmd = QR_get_command(res);

		if (retval == SQL_SUCCESS && cmd && EN_is_odbc3(env))
		{
			int	count;

			if (sscanf(cmd , "UPDATE %d", &count) == 1)
				;
			else if (sscanf(cmd , "DELETE %d", &count) == 1)
				;
			else
				count = -1;
			if (0 == count)
				retval = SQL_NO_DATA;
		}
#endif /* ODBCVER */
		stmt->diag_row_count = res->recent_processed_row_count;
	}
	/*
	 *	The cursor's info was changed ?
	 */
	if (retval == SQL_SUCCESS &&
	    (stmt->options.cursor_type != cursor_type ||
	     stmt->options.scroll_concurrency != scroll_concurrency))
	{
		SC_set_error(stmt, STMT_OPTION_VALUE_CHANGED, "cursor updatability changed");
		retval = SQL_SUCCESS_WITH_INFO;
	}
	return retval;
}

/*	Execute a prepared SQL statement */
RETCODE		SQL_API
PGAPI_Execute(HSTMT hstmt, UWORD flag)
{
	CSTR func = "PGAPI_Execute";
	StatementClass *stmt = (StatementClass *) hstmt;
	APDFields	*apdopts;
	IPDFields	*ipdopts;
	int			i,
				retval, start_row, end_row;
	BOOL	exec_end, recycled = FALSE, recycle = TRUE;

	mylog("%s: entering...\n", func);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		mylog("%s: NULL statement so return SQL_INVALID_HANDLE\n", func);
		return SQL_INVALID_HANDLE;
	}

	apdopts = SC_get_APDF(stmt);
	/*
	 * If the statement is premature, it means we already executed it from
	 * an SQLPrepare/SQLDescribeCol type of scenario.  So just return
	 * success.
	 */
	if (stmt->prepare && stmt->status == STMT_PREMATURE)
	{
		if (stmt->inaccurate_result)
		{
			stmt->exec_current_row = -1;
			SC_recycle_statement(stmt);
		}
		else
		{
			stmt->status = STMT_FINISHED;
			if (NULL == SC_get_errormsg(stmt))
			{
				mylog("%s: premature statement but return SQL_SUCCESS\n", func);
				return SQL_SUCCESS;
			}
			else
			{
				SC_log_error(func, "", stmt);
				mylog("%s: premature statement so return SQL_ERROR\n", func);
				return SQL_ERROR;
			}
		}
	}

	mylog("%s: clear errors...\n", func);

	SC_clear_error(stmt);

	if (!stmt->statement)
	{
		SC_set_error(stmt, STMT_NO_STMTSTRING, "This handle does not have a SQL statement stored in it");
		SC_log_error(func, "", stmt);
		mylog("%s: problem with handle\n", func);
		return SQL_ERROR;
	}

	if (stmt->exec_current_row > 0)
	{
		/*
		 * executing an array of parameters.
		 * Don't recycle the statement.
		 */
		recycle = FALSE;
	}
	else if (stmt->prepared)
	{
		QResultClass	*res;

		/*
		 * re-executing an prepared statement.
		 * Don't recycle the statement but
		 * discard the old result.
		 */
		recycle = FALSE;
		if (res = SC_get_Result(stmt), res)
		{
        		QR_Destructor(res);
        		SC_set_Result(stmt, NULL);
		}
	}
	/*
	 * If SQLExecute is being called again, recycle the statement. Note
	 * this should have been done by the application in a call to
	 * SQLFreeStmt(SQL_CLOSE) or SQLCancel.
	 */
	else if (stmt->status == STMT_FINISHED)
	{
		mylog("%s: recycling statement (should have been done by app)...\n", func);
/******** Is this really NEEDED ? ******/
		SC_recycle_statement(stmt);
		recycled = TRUE;
	}
	/* Check if the statement is in the correct state */
	else if ((stmt->prepare && stmt->status != STMT_READY) ||
		(stmt->status != STMT_ALLOCATED && stmt->status != STMT_READY))
	{
		SC_set_error(stmt, STMT_STATUS_ERROR, "The handle does not point to a statement that is ready to be executed");
		SC_log_error(func, "", stmt);
		mylog("%s: problem with statement\n", func);
		return SQL_ERROR;
	}

	if (start_row = stmt->exec_start_row, start_row < 0)
		start_row = 0; 
	if (end_row = stmt->exec_end_row, end_row < 0)
		end_row = apdopts->paramset_size - 1; 
	if (stmt->exec_current_row < 0)
		stmt->exec_current_row = start_row;
	ipdopts = SC_get_IPDF(stmt);
	if (stmt->exec_current_row == start_row)
	{
		if (ipdopts->param_processed_ptr)
			*ipdopts->param_processed_ptr = 0;
#if (ODBCVER >= 0x0300)
		/*
	 	 *	Initialize the param_status_ptr 
	 	 */
		if (ipdopts->param_status_ptr)
		{
			for (i = 0; i <= end_row; i++)
				ipdopts->param_status_ptr[i] = SQL_PARAM_UNUSED;
		}
#endif /* ODBCVER */
		if (recycle && !recycled)
			SC_recycle_statement(stmt);
	}

next_param_row:
#if (ODBCVER >= 0x0300)
	if (apdopts->param_operation_ptr)
	{
		while (apdopts->param_operation_ptr[stmt->exec_current_row] == SQL_PARAM_IGNORE)
		{
			if (stmt->exec_current_row >= end_row)
			{
				stmt->exec_current_row = -1;
				return SQL_SUCCESS;
			}
			++stmt->exec_current_row;
		}
	}
	/*
	 *	Initialize the current row status 
	 */
	if (ipdopts->param_status_ptr)
		ipdopts->param_status_ptr[stmt->exec_current_row] = SQL_PARAM_ERROR;
#endif /* ODBCVER */
	/*
	 * Check if statement has any data-at-execute parameters when it is
	 * not in SC_pre_execute.
	 */
	if (!stmt->pre_executing)
	{
		/*
		 * The bound parameters could have possibly changed since the last
		 * execute of this statement?  Therefore check for params and
		 * re-copy.
		 */
		UInt4	offset = apdopts->param_offset_ptr ? *apdopts->param_offset_ptr : 0;
		Int4	bind_size = apdopts->param_bind_type;
		Int4	current_row = stmt->exec_current_row < 0 ? 0 : stmt->exec_current_row;

		/*
		 *	Increment the  number of currently processed rows 
		 */
		if (ipdopts->param_processed_ptr)
			(*ipdopts->param_processed_ptr)++;
		stmt->data_at_exec = -1;
		for (i = 0; i < apdopts->allocated; i++)
		{
			Int4	   *pcVal = apdopts->parameters[i].used;

			apdopts->parameters[i].data_at_exec = FALSE;
			if (pcVal)
			{
				if (bind_size > 0)
					pcVal = (Int4 *)((char *)pcVal + offset + bind_size * current_row);
				else
					pcVal = (Int4 *)((char *)pcVal + offset + sizeof(SDWORD) * current_row);
				if (*pcVal == SQL_DATA_AT_EXEC || *pcVal <= SQL_LEN_DATA_AT_EXEC_OFFSET)
					apdopts->parameters[i].data_at_exec = TRUE;
			}
			/* Check for data at execution parameters */
			if (apdopts->parameters[i].data_at_exec)
			{
				if (stmt->data_at_exec < 0)
					stmt->data_at_exec = 1;
				else
					stmt->data_at_exec++;
			}
		}

		/*
		 * If there are some data at execution parameters, return need
		 * data
		 */

		/*
		 * SQLParamData and SQLPutData will be used to send params and
		 * execute the statement.
		 */
		if (stmt->data_at_exec > 0)
			return SQL_NEED_DATA;

	}

	retval = Exec_with_parameters_resolved(stmt, &exec_end);
	if (!exec_end)
		goto next_param_row;
	return retval;
}


RETCODE		SQL_API
PGAPI_Transact(
			   HENV henv,
			   HDBC hdbc,
			   UWORD fType)
{
	CSTR func = "PGAPI_Transact";
	extern ConnectionClass *conns[];
	ConnectionClass *conn;
	QResultClass *res;
	char		ok,
			   *stmt_string;
	int			lf;

	mylog("entering %s: hdbc=%u, henv=%u\n", func, hdbc, henv);

	if (hdbc == SQL_NULL_HDBC && henv == SQL_NULL_HENV)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	/*
	 * If hdbc is null and henv is valid, it means transact all
	 * connections on that henv.
	 */
	if (hdbc == SQL_NULL_HDBC && henv != SQL_NULL_HENV)
	{
		for (lf = 0; lf < MAX_CONNECTIONS; lf++)
		{
			conn = conns[lf];

			if (conn && conn->henv == henv)
				if (PGAPI_Transact(henv, (HDBC) conn, fType) != SQL_SUCCESS)
					return SQL_ERROR;
		}
		return SQL_SUCCESS;
	}

	conn = (ConnectionClass *) hdbc;

	if (fType == SQL_COMMIT)
		stmt_string = "COMMIT";
	else if (fType == SQL_ROLLBACK)
		stmt_string = "ROLLBACK";
	else
	{
		CC_set_error(conn, CONN_INVALID_ARGUMENT_NO, "PGAPI_Transact can only be called with SQL_COMMIT or SQL_ROLLBACK as parameter");
		CC_log_error(func, "", conn);
		return SQL_ERROR;
	}

	/* If manual commit and in transaction, then proceed. */
	if (!CC_is_in_autocommit(conn) && CC_is_in_trans(conn))
	{
		mylog("PGAPI_Transact: sending on conn %d '%s'\n", conn, stmt_string);

		res = CC_send_query(conn, stmt_string, NULL, CLEAR_RESULT_ON_ABORT);
		if (!res)
		{
			/* error msg will be in the connection */
			CC_on_abort(conn, NO_TRANS);
			CC_log_error(func, "", conn);
			return SQL_ERROR;
		}

		ok = QR_command_maybe_successful(res);
		QR_Destructor(res);

		if (!ok)
		{
			CC_on_abort(conn, NO_TRANS);
			CC_log_error(func, "", conn);
			return SQL_ERROR;
		}
	}
	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_Cancel(
			 HSTMT hstmt)		/* Statement to cancel. */
{
	CSTR func = "PGAPI_Cancel";
	StatementClass *stmt = (StatementClass *) hstmt;
	ConnectionClass *conn;
	RETCODE		result;
	ConnInfo   *ci;

#ifdef WIN32
	HMODULE		hmodule;
	FARPROC		addr;
#endif

	mylog("%s: entering...\n", func);

	/* Check if this can handle canceling in the middle of a SQLPutData? */
	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
	conn = SC_get_conn(stmt);
	ci = &(conn->connInfo);

	/*
	 * Not in the middle of SQLParamData/SQLPutData so cancel like a
	 * close.
	 */
	if (stmt->data_at_exec < 0)
	{
		/*
		 * Tell the Backend that we're cancelling this request
		 */
		if (stmt->status == STMT_EXECUTING)
			CC_send_cancel_request(conn);
		/*
		 * MAJOR HACK for Windows to reset the driver manager's cursor
		 * state: Because of what seems like a bug in the Odbc driver
		 * manager, SQLCancel does not act like a SQLFreeStmt(CLOSE), as
		 * many applications depend on this behavior.  So, this brute
		 * force method calls the driver manager's function on behalf of
		 * the application.
		 */

#ifdef WIN32
		if (ci->drivers.cancel_as_freestmt)
		{
			hmodule = GetModuleHandle("ODBC32");
			addr = GetProcAddress(hmodule, "SQLFreeStmt");
			result = addr((char *) (stmt->phstmt) - 96, SQL_CLOSE);
		}
		else
			result = PGAPI_FreeStmt(hstmt, SQL_CLOSE);
#else
		result = PGAPI_FreeStmt(hstmt, SQL_CLOSE);
#endif

		mylog("PGAPI_Cancel:  PGAPI_FreeStmt returned %d\n", result);

		SC_clear_error(hstmt);
		return SQL_SUCCESS;
	}

	/* In the middle of SQLParamData/SQLPutData, so cancel that. */

	/*
	 * Note, any previous data-at-exec buffers will be freed in the
	 * recycle
	 */
	/* if they call SQLExecDirect or SQLExecute again. */

	stmt->data_at_exec = -1;
	stmt->current_exec_param = -1;
	stmt->put_data = FALSE;
	cancelNeedDataState(stmt);

	return SQL_SUCCESS;
}


/*
 *	Returns the SQL string as modified by the driver.
 *	Currently, just copy the input string without modification
 *	observing buffer limits and truncation.
 */
RETCODE		SQL_API
PGAPI_NativeSql(
				HDBC hdbc,
				UCHAR FAR * szSqlStrIn,
				SDWORD cbSqlStrIn,
				UCHAR FAR * szSqlStr,
				SDWORD cbSqlStrMax,
				SDWORD FAR * pcbSqlStr)
{
	CSTR func = "PGAPI_NativeSql";
	int			len = 0;
	char	   *ptr;
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	RETCODE		result;

	mylog("%s: entering...cbSqlStrIn=%d\n", func, cbSqlStrIn);

	ptr = (cbSqlStrIn == 0) ? "" : make_string(szSqlStrIn, cbSqlStrIn, NULL, 0);
	if (!ptr)
	{
		CC_set_error(conn, CONN_NO_MEMORY_ERROR, "No memory available to store native sql string");
		CC_log_error(func, "", conn);
		return SQL_ERROR;
	}

	result = SQL_SUCCESS;
	len = strlen(ptr);

	if (szSqlStr)
	{
		strncpy_null(szSqlStr, ptr, cbSqlStrMax);

		if (len >= cbSqlStrMax)
		{
			result = SQL_SUCCESS_WITH_INFO;
			CC_set_error(conn, STMT_TRUNCATED, "The buffer was too small for the NativeSQL.");
		}
	}

	if (pcbSqlStr)
		*pcbSqlStr = len;

	if (cbSqlStrIn)
		free(ptr);

	return result;
}


/*
 *	Supplies parameter data at execution time.
 *	Used in conjuction with SQLPutData.
 */
RETCODE		SQL_API
PGAPI_ParamData(
				HSTMT hstmt,
				PTR FAR * prgbValue)
{
	CSTR func = "PGAPI_ParamData";
	StatementClass *stmt = (StatementClass *) hstmt, *estmt;
	APDFields	*apdopts;
	IPDFields	*ipdopts;
	RETCODE		retval;
	int		i;
	ConnInfo   *ci;

	mylog("%s: entering...\n", func);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
	ci = &(SC_get_conn(stmt)->connInfo);

	estmt = stmt->execute_delegate ? stmt->execute_delegate : stmt;
	apdopts = SC_get_APDF(estmt);
	mylog("%s: data_at_exec=%d, params_alloc=%d\n", func, estmt->data_at_exec, apdopts->allocated);

	if (estmt->data_at_exec < 0)
	{
		SC_set_error(stmt, STMT_SEQUENCE_ERROR, "No execution-time parameters for this statement");
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (estmt->data_at_exec > apdopts->allocated)
	{
		SC_set_error(stmt, STMT_SEQUENCE_ERROR, "Too many execution-time parameters were present");
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/* close the large object */
	if (estmt->lobj_fd >= 0)
	{
		lo_close(estmt->hdbc, estmt->lobj_fd);

		/* commit transaction if needed */
		if (!ci->drivers.use_declarefetch && CC_is_in_autocommit(estmt->hdbc))
		{
			if (!CC_commit(estmt->hdbc))
			{
				SC_set_error(stmt, STMT_EXEC_ERROR, "Could not commit (in-line) a transaction");
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}
		}
		estmt->lobj_fd = -1;
	}

	/* Done, now copy the params and then execute the statement */
	ipdopts = SC_get_IPDF(estmt);
	if (estmt->data_at_exec == 0)
	{
		BOOL	exec_end;

		retval = Exec_with_parameters_resolved(estmt, &exec_end);
		if (exec_end)
		{
			stmt->execute_delegate = NULL;
			return dequeueNeedDataCallback(retval, stmt);
		}
		if (retval = PGAPI_Execute(estmt, 0), SQL_NEED_DATA != retval)
			return retval;
	}

	/*
	 * Set beginning param;  if first time SQLParamData is called , start
	 * at 0. Otherwise, start at the last parameter + 1.
	 */
	i = estmt->current_exec_param >= 0 ? estmt->current_exec_param + 1 : 0;

	/* At least 1 data at execution parameter, so Fill in the token value */
	for (; i < apdopts->allocated; i++)
	{
		if (apdopts->parameters[i].data_at_exec)
		{
			estmt->data_at_exec--;
			estmt->current_exec_param = i;
			estmt->put_data = FALSE;
			if (prgbValue)
			{
				/* returns token here */
				if (stmt->execute_delegate)
				{
					UInt4	offset = apdopts->param_offset_ptr ? *apdopts->param_offset_ptr : 0;
					UInt4	perrow = apdopts->param_bind_type > 0 ? apdopts->param_bind_type : apdopts->parameters[i].buflen; 

					*prgbValue = apdopts->parameters[i].buffer + offset + estmt->exec_current_row * perrow;
				}
				else
					*prgbValue = apdopts->parameters[i].buffer;
			}
			break;
		}
	}

	return SQL_NEED_DATA;
}


/*
 *	Supplies parameter data at execution time.
 *	Used in conjunction with SQLParamData.
 */
RETCODE		SQL_API
PGAPI_PutData(
			  HSTMT hstmt,
			  PTR rgbValue,
			  SDWORD cbValue)
{
	CSTR func = "PGAPI_PutData";
	StatementClass *stmt = (StatementClass *) hstmt, *estmt;
	ConnectionClass *conn;
	APDFields	*apdopts;
	IPDFields	*ipdopts;
	PutDataInfo	*pdata;
	int			old_pos,
				retval;
	ParameterInfoClass *current_param;
	ParameterImplClass *current_iparam;
	PutDataClass	*current_pdata;
	char	   *buffer, *putbuf, *allocbuf = NULL;
	Int2		ctype;
	SDWORD          putlen;
	BOOL		lenset = FALSE;

	mylog("%s: entering...\n", func);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	estmt = stmt->execute_delegate ? stmt->execute_delegate : stmt;
	apdopts = SC_get_APDF(estmt);
	if (estmt->current_exec_param < 0)
	{
		SC_set_error(stmt, STMT_SEQUENCE_ERROR, "Previous call was not SQLPutData or SQLParamData");
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	current_param = &(apdopts->parameters[estmt->current_exec_param]);
	ipdopts = SC_get_IPDF(estmt);
	current_iparam = &(ipdopts->parameters[estmt->current_exec_param]);
	pdata = SC_get_PDTI(estmt);
	current_pdata = &(pdata->pdata[estmt->current_exec_param]);
	ctype = current_param->CType;

	conn = SC_get_conn(estmt);
	if (ctype == SQL_C_DEFAULT)
		ctype = sqltype_to_default_ctype(conn, current_iparam->SQLType);
	if (SQL_NTS == cbValue)
	{
#ifdef	UNICODE_SUPPORT
		if (SQL_C_WCHAR == ctype)
		{
			putlen = WCLEN * ucs2strlen((SQLWCHAR *) rgbValue);
			lenset = TRUE;
		}
		else
#endif /* UNICODE_SUPPORT */
		if (SQL_C_CHAR == ctype)
		{
			putlen = strlen(rgbValue);
			lenset = TRUE;
		}
	}
	if (!lenset)
	{
		if (cbValue < 0)
			putlen = cbValue;
		else
#ifdef	UNICODE_SUPPORT
		if (ctype == SQL_C_CHAR || ctype == SQL_C_BINARY || ctype == SQL_C_WCHAR)
#else
		if (ctype == SQL_C_CHAR || ctype == SQL_C_BINARY)
#endif /* UNICODE_SUPPORT */
			putlen = cbValue;
		else
			putlen = ctype_length(ctype);
	}
	putbuf = rgbValue;
	if (current_iparam->PGType == conn->lobj_type && SQL_C_CHAR == ctype)
	{
		allocbuf = malloc(putlen / 2 + 1);
		if (allocbuf)
		{
			pg_hex2bin(rgbValue, allocbuf, putlen);
			putbuf = allocbuf;
			putlen /= 2;
		}
	}

	if (!estmt->put_data)
	{							/* first call */
		mylog("PGAPI_PutData: (1) cbValue = %d\n", cbValue);

		estmt->put_data = TRUE;

		current_pdata->EXEC_used = (SDWORD *) malloc(sizeof(SDWORD));
		if (!current_pdata->EXEC_used)
		{
			SC_set_error(stmt, STMT_NO_MEMORY_ERROR, "Out of memory in PGAPI_PutData (1)");
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		*current_pdata->EXEC_used = putlen;

		if (cbValue == SQL_NULL_DATA)
			return SQL_SUCCESS;

		/* Handle Long Var Binary with Large Objects */
		/* if (current_iparam->SQLType == SQL_LONGVARBINARY) */
		if (current_iparam->PGType == conn->lobj_type)
		{
			/* begin transaction if needed */
			if (!CC_is_in_trans(conn))
			{
				if (!CC_begin(conn))
				{
					SC_set_error(stmt, STMT_EXEC_ERROR, "Could not begin (in-line) a transaction");
					SC_log_error(func, "", stmt);
					return SQL_ERROR;
				}
			}

			/* store the oid */
			current_pdata->lobj_oid = lo_creat(conn, INV_READ | INV_WRITE);
			if (current_pdata->lobj_oid == 0)
			{
				SC_set_error(stmt, STMT_EXEC_ERROR, "Couldnt create large object.");
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}

			/*
			 * major hack -- to allow convert to see somethings there have
			 * to modify convert to handle this better
			 */
			/***current_param->EXEC_buffer = (char *) &current_param->lobj_oid;***/

			/* store the fd */
			estmt->lobj_fd = lo_open(conn, current_pdata->lobj_oid, INV_WRITE);
			if (estmt->lobj_fd < 0)
			{
				SC_set_error(stmt, STMT_EXEC_ERROR, "Couldnt open large object for writing.");
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}

			retval = lo_write(conn, estmt->lobj_fd, putbuf, putlen);
			mylog("lo_write: cbValue=%d, wrote %d bytes\n", putlen, retval);
		}
		else
		{
			current_pdata->EXEC_buffer = malloc(putlen + 1);
			if (!current_pdata->EXEC_buffer)
			{
				SC_set_error(stmt, STMT_NO_MEMORY_ERROR, "Out of memory in PGAPI_PutData (2)");
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}
			memcpy(current_pdata->EXEC_buffer, putbuf, putlen);
			current_pdata->EXEC_buffer[putlen] = '\0';
		}
	}
	else
	{
		/* calling SQLPutData more than once */
		mylog("PGAPI_PutData: (>1) cbValue = %d\n", cbValue);

		/* if (current_iparam->SQLType == SQL_LONGVARBINARY) */
		if (current_iparam->PGType == conn->lobj_type)
		{
			/* the large object fd is in EXEC_buffer */
			retval = lo_write(conn, estmt->lobj_fd, putbuf, putlen);
			mylog("lo_write(2): cbValue = %d, wrote %d bytes\n", putlen, retval);

			*current_pdata->EXEC_used += putlen;
		}
		else
		{
			buffer = current_pdata->EXEC_buffer;
			old_pos = *current_pdata->EXEC_used;
			if (putlen > 0)
			{
				*current_pdata->EXEC_used += putlen;

				mylog("        cbValue = %d, old_pos = %d, *used = %d\n", putlen, old_pos, *current_pdata->EXEC_used);

				/* dont lose the old pointer in case out of memory */
				buffer = realloc(current_pdata->EXEC_buffer, *current_pdata->EXEC_used + 1);
				if (!buffer)
				{
					SC_set_error(stmt, STMT_NO_MEMORY_ERROR,"Out of memory in PGAPI_PutData (3)");
					SC_log_error(func, "", stmt);
					return SQL_ERROR;
				}

				memcpy(&buffer[old_pos], putbuf, putlen);
				buffer[*current_pdata->EXEC_used] = '\0';

				/* reassign buffer incase realloc moved it */
				current_pdata->EXEC_buffer = buffer;
			}
			else
			{
				SC_log_error(func, "bad cbValue", stmt);
				return SQL_ERROR;
			}
		}
	}
	if (allocbuf)
		free(allocbuf);

	return SQL_SUCCESS;
}
