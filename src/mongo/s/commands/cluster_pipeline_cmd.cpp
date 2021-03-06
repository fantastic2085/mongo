/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/s/query/cluster_aggregate.h"

namespace mongo {
namespace {

class ClusterPipelineCommand final : public Command {
public:
    ClusterPipelineCommand() : Command("aggregate") {}

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        // TODO: Parsing to a Pipeline and/or AggregationRequest here.
        return std::make_unique<Invocation>(this, opMsgRequest);
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(Command* cmd, const OpMsgRequest& request)
            : CommandInvocation(cmd),
              _request(request),
              _dbName(request.getDatabase().toString()) {}

    private:
        bool supportsWriteConcern() const override {
            return Pipeline::aggSupportsWriteConcern(_request.body);
        }

        bool supportsReadConcern(repl::ReadConcernLevel level) const override {
            return true;
        }

        static void _runAggCommand(OperationContext* opCtx,
                                   const std::string& dbname,
                                   const BSONObj& cmdObj,
                                   boost::optional<ExplainOptions::Verbosity> verbosity,
                                   BSONObjBuilder* result) {
            const auto aggregationRequest =
                uassertStatusOK(AggregationRequest::parseFromBSON(dbname, cmdObj, verbosity));

            const auto& nss = aggregationRequest.getNamespaceString();

            try {
                uassertStatusOK(
                    ClusterAggregate::runAggregate(opCtx,
                                                   ClusterAggregate::Namespaces{nss, nss},
                                                   aggregationRequest,
                                                   cmdObj,
                                                   result));
            } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
                // If the aggregation failed because the namespace is a view, re-run the command
                // with the resolved view pipeline and namespace.
                uassertStatusOK(ClusterAggregate::retryOnViewError(
                    opCtx, aggregationRequest, *ex.extraInfo<ResolvedView>(), nss, result));
            }
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            auto bob = reply->getBodyBuilder();
            _runAggCommand(opCtx, _dbName, _request.body, boost::none, &bob);
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            auto bodyBuilder = result->getBodyBuilder();
            _runAggCommand(opCtx, _dbName, _request.body, verbosity, &bodyBuilder);
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            const auto nss = ns();
            uassertStatusOK(AuthorizationSession::get(opCtx->getClient())
                                ->checkAuthForAggregate(nss, _request.body, true));
        }

        NamespaceString ns() const override {
            return AggregationRequest::parseNs(_dbName, _request.body);
        }

        const OpMsgRequest& _request;
        const std::string _dbName;
    };

    std::string help() const override {
        return "Runs the sharded aggregation command. See "
               "http://dochub.mongodb.org/core/aggregation for more details.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }
} clusterPipelineCmd;

}  // namespace
}  // namespace mongo
