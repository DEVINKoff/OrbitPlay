// SPDX-License-Identifier: AGPL-3.0-only
// HTTPS for PSN sign-in on Apple platforms (NSURLSession, system certificates and proxy).
#import <Foundation/Foundation.h>

#include <string>

bool HttpsRequest(const std::string &method, const std::string &path, const std::string &headers,
		const std::string &body, std::string *response, int *status)
{
	@autoreleasepool
	{
		NSString *url = [NSString stringWithFormat:@"https://auth.api.sonyentertainmentnetwork.com%s", path.c_str()];
		NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
		req.HTTPMethod = [NSString stringWithUTF8String:method.c_str()];
		req.timeoutInterval = 20;
		// "Name: value\r\n" lines
		size_t start = 0;
		while(start < headers.size())
		{
			size_t end = headers.find("\r\n", start);
			if(end == std::string::npos)
				end = headers.size();
			std::string line = headers.substr(start, end - start);
			size_t colon = line.find(':');
			if(colon != std::string::npos)
			{
				std::string name = line.substr(0, colon), value = line.substr(colon + 1);
				while(!value.empty() && value[0] == ' ')
					value.erase(0, 1);
				[req setValue:[NSString stringWithUTF8String:value.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];
			}
			start = end + 2;
		}
		if(!body.empty())
			req.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];

		__block bool ok = false;
		__block int code = 0;
		__block std::string out;
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		NSURLSessionDataTask *task = [[NSURLSession sharedSession] dataTaskWithRequest:req
			completionHandler:^(NSData *data, NSURLResponse *resp, NSError *error) {
				if(!error && [resp isKindOfClass:[NSHTTPURLResponse class]])
				{
					code = (int)((NSHTTPURLResponse *)resp).statusCode;
					if(data)
						out.assign((const char *)data.bytes, data.length);
					ok = true;
				}
				dispatch_semaphore_signal(done);
			}];
		[task resume];
		dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
		*status = code;
		*response = out;
		return ok;
	}
}
